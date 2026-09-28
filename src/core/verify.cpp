#include <fragfs/verify.h>

#include <fragfs/crc32.h>
#include <fragfs/error.h>
#include <fragfs/metadata_store.h>
#include <fragfs/posix_file.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <vector>

#include <unistd.h>

namespace fragfs {
namespace {

std::error_code checksumRange(int fd, uint64_t offset, uint64_t length,
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

VerifyReport verifyMetadata(const Metadata& metadata,
                            const std::filesystem::path& baseDirectory) {
    VerifyReport report;

    // Structural problems make the rest of the checks meaningless.
    const ValidationResult validation = metadata.validate();
    if (!validation.ok()) {
        report.metadataError = validation.error;
        return report;
    }

    report.fragments.reserve(metadata.fragments.size());

    for (std::size_t i = 0; i < metadata.fragments.size(); ++i) {
        const Fragment& fragment = metadata.fragments[i];
        FragmentVerification verification;
        verification.fragmentIndex = i;

        const std::filesystem::path resolved =
            resolvePhysicalPath(fragment.path, baseDirectory);

        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::status(resolved, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error && !std::filesystem::exists(status))) {
            verification.error = make_error_code(ErrorCode::missing_physical_file);
            ++report.missingFiles;
            report.fragments.push_back(verification);
            continue;
        }
        if (error) {
            verification.error = error;
            ++report.missingFiles;
            report.fragments.push_back(verification);
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            verification.error = make_error_code(ErrorCode::not_a_regular_file);
            ++report.invalidRanges;
            report.fragments.push_back(verification);
            continue;
        }

        verification.fileExists = true;

        auto opened = PosixFile::open(resolved, OpenFlags::Read, error);
        if (!opened) {
            verification.error = error ? error : make_error_code(ErrorCode::io_error);
            ++report.invalidRanges;
            report.fragments.push_back(verification);
            continue;
        }

        uint64_t fileSize = 0;
        error = opened->size(fileSize);
        if (error) {
            verification.error = error;
            ++report.invalidRanges;
            report.fragments.push_back(verification);
            continue;
        }
        verification.physicalFileSize = fileSize;

        const std::optional<uint64_t> physicalEnd = fragment.physicalEnd();
        if (!physicalEnd.has_value()) {
            verification.error = make_error_code(ErrorCode::invalid_range);
            ++report.invalidRanges;
            report.fragments.push_back(verification);
            continue;
        }
        if (*physicalEnd > fileSize) {
            verification.error = make_error_code(ErrorCode::physical_range_out_of_bounds);
            ++report.invalidRanges;
            report.fragments.push_back(verification);
            continue;
        }
        verification.rangeInBounds = true;

        // If the metadata recorded the file's identity, compare it now. A
        // difference in device/inode means the file was replaced; a difference
        // in size or mtime means it was modified.
        if (fragment.identity.has_value()) {
            FileIdentity current;
            error = opened->identity(current);
            if (error) {
                verification.error = error;
                ++report.invalidRanges;
                report.fragments.push_back(verification);
                continue;
            }
            if (current != *fragment.identity) {
                verification.changed = true;
                verification.error = make_error_code(ErrorCode::physical_file_changed);
                ++report.changedFiles;
            }
        }

        // If a content checksum was recorded (manifest v4), verify it.
        if (fragment.checksum.has_value()) {
            verification.checksumChecked = true;
            uint32_t actual = 0;
            error = checksumRange(opened->nativeHandle(), fragment.physicalStart,
                                  fragment.length, actual);
            if (error) {
                verification.error = error;
                ++report.invalidRanges;
                report.fragments.push_back(verification);
                continue;
            }
            if (actual == *fragment.checksum) {
                verification.checksumOk = true;
            } else {
                verification.error = make_error_code(ErrorCode::checksum_mismatch);
                ++report.corruptedFiles;
            }
        }

        report.fragments.push_back(verification);
    }

    return report;
}

} // namespace fragfs
