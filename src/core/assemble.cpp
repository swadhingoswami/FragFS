#include <fragfs/assemble.h>

#include <fragfs/crc32.h>
#include <fragfs/error.h>
#include <fragfs/metadata_store.h>
#include <fragfs/platform_io.h>
#include <fragfs/posix_file.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <unistd.h>

namespace fragfs {
namespace {

// Reads a range of a chunk and returns its CRC-32. Used to verify a chunk when
// reflink avoided reading it during assembly.
std::error_code checksumRange(int fd,
                              uint64_t offset,
                              uint64_t length,
                              uint32_t& checksum) {
    std::vector<char> buffer(1024 * 1024);
    Crc32 crc;
    uint64_t done = 0;
    while (done < length) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<uint64_t>(buffer.size(), length - done));
        const ssize_t got = ::pread(fd, buffer.data(), want,
                                    static_cast<off_t>(offset + done));
        if (got < 0) {
            return std::error_code(errno, std::generic_category());
        }
        if (got == 0) {
            break;
        }
        crc.update(buffer.data(), static_cast<std::size_t>(got));
        done += static_cast<uint64_t>(got);
    }
    checksum = crc.value();
    return {};
}

} // namespace

AssembleResult assemble(const Metadata& metadata,
                        const std::filesystem::path& baseDirectory,
                        const std::filesystem::path& outputPath,
                        const AssembleOptions& options) {
    AssembleResult result;

    const ValidationResult validation = metadata.validate();
    if (!validation.ok()) {
        result.error = validation.error;
        result.detail = validation.detail;
        return result;
    }

    const std::filesystem::path temporary(outputPath.string() + ".tmp");
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }

    std::error_code error;
    auto out = PosixFile::open(
        temporary, OpenFlags::Write | OpenFlags::Create | OpenFlags::Truncate, error);
    if (!out) {
        result.error = error ? error : make_error_code(ErrorCode::io_error);
        return result;
    }
    error = out->truncate(metadata.logicalSize);
    if (error) {
        result.error = error;
        return result;
    }

    bool reflinkPossible = true;
    bool usedReflink = false;
    bool anyChecksum = false;

    for (const Fragment& fragment : metadata.fragments) {
        const std::filesystem::path resolved =
            resolvePhysicalPath(fragment.path, baseDirectory);

        auto chunk = PosixFile::open(resolved, OpenFlags::Read, error);
        if (!chunk) {
            result.error = error == std::errc::no_such_file_or_directory
                               ? make_error_code(ErrorCode::missing_physical_file)
                               : (error ? error : make_error_code(ErrorCode::io_error));
            result.detail = fragment.path;
            return result;
        }

        uint64_t chunkSize = 0;
        error = chunk->size(chunkSize);
        if (error) {
            result.error = error;
            result.detail = fragment.path;
            return result;
        }
        const std::optional<uint64_t> physicalEnd = fragment.physicalEnd();
        if (!physicalEnd.has_value() || *physicalEnd > chunkSize) {
            result.error = make_error_code(ErrorCode::physical_range_out_of_bounds);
            result.detail = fragment.path;
            return result;
        }

        // Fast path: clone the range (no data movement). If the filesystem
        // does not support it, fall through to a copy for this and later
        // fragments.
        if (reflinkPossible && !options.verify) {
            const std::error_code cloneError =
                cloneRange(out->nativeHandle(), fragment.logicalStart,
                           chunk->nativeHandle(), fragment.physicalStart,
                           fragment.length);
            if (!cloneError) {
                usedReflink = true;
                continue;
            }
            if (cloneError == std::errc::not_supported ||
                cloneError == std::errc::operation_not_supported) {
                reflinkPossible = false;
            } else {
                result.error = cloneError;
                result.detail = fragment.path;
                return result;
            }
        } else if (reflinkPossible && options.verify) {
            // Verifying requires reading, which negates reflink's benefit, so
            // use the copy path (which verifies as it copies).
            reflinkPossible = false;
        }

        // Copy path. When a checksum is recorded, verify it in the same pass.
        if (fragment.checksum.has_value()) {
            anyChecksum = true;
            Crc32 crc;
            std::vector<char> buffer(1024 * 1024);
            uint64_t done = 0;
            while (done < fragment.length) {
                const std::size_t want = static_cast<std::size_t>(
                    std::min<uint64_t>(buffer.size(), fragment.length - done));
                std::size_t got = 0;
                error = chunk->pread(fragment.physicalStart + done, buffer.data(),
                                     want, got);
                if (error) {
                    result.error = error;
                    result.detail = fragment.path;
                    return result;
                }
                if (got == 0) {
                    result.error = make_error_code(ErrorCode::physical_range_out_of_bounds);
                    result.detail = fragment.path;
                    return result;
                }
                std::size_t put = 0;
                error = out->pwrite(fragment.logicalStart + done, buffer.data(), got,
                                    put);
                if (error) {
                    result.error = error;
                    result.detail = fragment.path;
                    return result;
                }
                if (put != got) {
                    result.error = make_error_code(ErrorCode::io_error);
                    result.detail = fragment.path;
                    return result;
                }
                crc.update(buffer.data(), got);
                done += got;
            }
            if (crc.value() != *fragment.checksum) {
                result.error = make_error_code(ErrorCode::checksum_mismatch);
                result.detail = fragment.path;
                return result;
            }
        } else {
            uint64_t copied = 0;
            error = copyRange(out->nativeHandle(), fragment.logicalStart,
                              chunk->nativeHandle(), fragment.physicalStart,
                              fragment.length, copied);
            if (error) {
                result.error = error;
                result.detail = fragment.path;
                return result;
            }
            if (copied != fragment.length) {
                result.error = make_error_code(ErrorCode::physical_range_out_of_bounds);
                result.detail = fragment.path;
                return result;
            }
        }
    }

    // If reflink was used and verification was requested, read each chunk back
    // and check its CRC (a separate pass, since cloning did not read).
    if (options.verify && usedReflink) {
        for (const Fragment& fragment : metadata.fragments) {
            if (!fragment.checksum.has_value()) {
                continue;
            }
            anyChecksum = true;
            const std::filesystem::path resolved =
                resolvePhysicalPath(fragment.path, baseDirectory);
            auto chunk = PosixFile::open(resolved, OpenFlags::Read, error);
            if (!chunk) {
                result.error = error ? error : make_error_code(ErrorCode::io_error);
                result.detail = fragment.path;
                return result;
            }
            uint32_t actual = 0;
            error = checksumRange(chunk->nativeHandle(), fragment.physicalStart,
                                  fragment.length, actual);
            if (error) {
                result.error = error;
                result.detail = fragment.path;
                return result;
            }
            if (actual != *fragment.checksum) {
                result.error = make_error_code(ErrorCode::checksum_mismatch);
                result.detail = fragment.path;
                return result;
            }
        }
    }

    error = out->sync();
    if (error) {
        result.error = error;
        return result;
    }

    std::filesystem::rename(temporary, outputPath, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        result.error = error;
        return result;
    }
    (void)syncDirectory(baseDirectory);

    if (options.consume) {
        for (const Fragment& fragment : metadata.fragments) {
            const std::filesystem::path resolved =
                resolvePhysicalPath(fragment.path, baseDirectory);
            std::error_code ignored;
            std::filesystem::remove(resolved, ignored);
        }
    }

    result.usedReflink = usedReflink;
    result.checksumsVerified = anyChecksum;
    return result;
}

} // namespace fragfs
