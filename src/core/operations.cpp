#include <fragfs/operations.h>

#include <fragfs/checked_arithmetic.h>
#include <fragfs/error.h>
#include <fragfs/metadata_store.h>
#include <fragfs/posix_file.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace fragfs {
namespace {

// Stats a physical file and returns its identity (size, device, inode, mtime).
// Distinguishes a missing path, a non-regular path, and an unreadable file.
std::error_code statPhysicalFile(const std::filesystem::path& file, FileIdentity& out) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(file, error);
    if (error == std::errc::no_such_file_or_directory) {
        return make_error_code(ErrorCode::missing_physical_file);
    }
    if (error) {
        return error;
    }
    if (!std::filesystem::exists(status)) {
        return make_error_code(ErrorCode::missing_physical_file);
    }
    if (!std::filesystem::is_regular_file(status)) {
        return make_error_code(ErrorCode::not_a_regular_file);
    }

    auto opened = PosixFile::open(file, OpenFlags::Read, error);
    if (!opened) {
        return error ? error : make_error_code(ErrorCode::io_error);
    }
    return opened->identity(out);
}

std::error_code validateResult(const Metadata& metadata) {
    return metadata.validate().error;
}

std::string zeroPadded(uint64_t value, std::size_t width) {
    std::string digits = std::to_string(value);
    if (digits.size() < width) {
        digits.insert(0, width - digits.size(), '0');
    }
    return digits;
}

} // namespace

CreateResult buildMetadata(const std::vector<std::filesystem::path>& files,
                           const std::filesystem::path& metadataPath) {
    CreateResult result;
    const std::filesystem::path baseDirectory = baseDirectoryFor(metadataPath);

    uint64_t logicalStart = 0;
    for (const std::filesystem::path& file : files) {
        FileIdentity identity;
        result.error = statPhysicalFile(file, identity);
        if (result.error) {
            return result;
        }
        if (identity.size == 0) {
            result.error = make_error_code(ErrorCode::empty_physical_file);
            return result;
        }

        const std::optional<uint64_t> logicalEnd =
            checkedAdd(logicalStart, identity.size);
        if (!logicalEnd.has_value()) {
            result.error = make_error_code(ErrorCode::invalid_range);
            return result;
        }

        Fragment fragment;
        fragment.logicalStart = logicalStart;
        fragment.physicalStart = 0;
        fragment.length = identity.size;
        fragment.path = storePath(file, baseDirectory).string();
        fragment.identity = identity;
        result.metadata.fragments.push_back(std::move(fragment));

        logicalStart = *logicalEnd;
    }

    result.metadata.logicalSize = logicalStart;
    result.error = validateResult(result.metadata);
    return result;
}

std::error_code appendFile(Metadata& metadata,
                           const std::filesystem::path& file,
                           const std::filesystem::path& metadataPath) {
    FileIdentity identity;
    std::error_code error = statPhysicalFile(file, identity);
    if (error) {
        return error;
    }
    if (identity.size == 0) {
        return make_error_code(ErrorCode::empty_physical_file);
    }

    const std::optional<uint64_t> logicalEnd =
        checkedAdd(metadata.logicalSize, identity.size);
    if (!logicalEnd.has_value()) {
        return make_error_code(ErrorCode::invalid_range);
    }

    Fragment fragment;
    fragment.logicalStart = metadata.logicalSize;
    fragment.physicalStart = 0;
    fragment.length = identity.size;
    fragment.path = storePath(file, baseDirectoryFor(metadataPath)).string();
    fragment.identity = identity;
    metadata.fragments.push_back(std::move(fragment));
    metadata.logicalSize = *logicalEnd;

    return validateResult(metadata);
}

std::error_code addRange(Metadata& metadata,
                         const std::filesystem::path& file,
                         uint64_t physicalOffset,
                         uint64_t length,
                         const std::filesystem::path& metadataPath) {
    if (length == 0) {
        return make_error_code(ErrorCode::invalid_range);
    }

    uint64_t fileSize = 0;
    FileIdentity identity;
    std::error_code error = statPhysicalFile(file, identity);
    if (error) {
        return error;
    }
    fileSize = identity.size;

    const std::optional<uint64_t> physicalEnd = checkedAdd(physicalOffset, length);
    if (!physicalEnd.has_value()) {
        return make_error_code(ErrorCode::invalid_range);
    }
    if (*physicalEnd > fileSize) {
        return make_error_code(ErrorCode::physical_range_out_of_bounds);
    }

    const std::optional<uint64_t> logicalEnd = checkedAdd(metadata.logicalSize, length);
    if (!logicalEnd.has_value()) {
        return make_error_code(ErrorCode::invalid_range);
    }

    Fragment fragment;
    fragment.logicalStart = metadata.logicalSize;
    fragment.physicalStart = physicalOffset;
    fragment.length = length;
    fragment.path = storePath(file, baseDirectoryFor(metadataPath)).string();
    fragment.identity = identity;
    metadata.fragments.push_back(std::move(fragment));
    metadata.logicalSize = *logicalEnd;

    return validateResult(metadata);
}

std::error_code removeFragment(Metadata& metadata, std::size_t fragmentIndex) {
    if (fragmentIndex >= metadata.fragments.size()) {
        return make_error_code(ErrorCode::fragment_index_out_of_range);
    }

    const uint64_t removedLength = metadata.fragments[fragmentIndex].length;
    metadata.fragments.erase(
        metadata.fragments.begin() + static_cast<std::ptrdiff_t>(fragmentIndex));

    // Later fragments keep their physical mapping; only their logical start
    // moves down to close the gap left by the removed fragment.
    for (std::size_t i = fragmentIndex; i < metadata.fragments.size(); ++i) {
        metadata.fragments[i].logicalStart -= removedLength;
    }
    metadata.logicalSize -= removedLength;

    return validateResult(metadata);
}

SplitResult splitFile(const std::filesystem::path& input,
                      const std::filesystem::path& outputPrefix,
                      uint64_t chunkSize) {
    SplitResult result;
    if (chunkSize == 0) {
        result.error = make_error_code(ErrorCode::invalid_range);
        return result;
    }

    std::error_code error;
    auto in = PosixFile::open(input, OpenFlags::Read, error);
    if (!in) {
        if (error == std::errc::no_such_file_or_directory) {
            result.error = make_error_code(ErrorCode::missing_physical_file);
        } else {
            result.error = error ? error : make_error_code(ErrorCode::io_error);
        }
        return result;
    }

    uint64_t size = 0;
    error = in->size(size);
    if (error) {
        result.error = error;
        return result;
    }
    if (size == 0) {
        result.error = make_error_code(ErrorCode::empty_physical_file);
        return result;
    }

    // Number of parts and the zero-padding width for their names.
    const uint64_t partCount = (size + chunkSize - 1) / chunkSize;
    std::size_t width = std::to_string(partCount - 1).size();
    if (width < 3) {
        width = 3;
    }

    std::vector<char> buffer(1024 * 1024);
    uint64_t offset = 0;
    for (uint64_t part = 0; part < partCount; ++part) {
        const uint64_t thisSize = std::min(chunkSize, size - offset);
        const std::filesystem::path partPath(
            outputPrefix.string() + zeroPadded(part, width));

        auto out = PosixFile::open(
            partPath, OpenFlags::Write | OpenFlags::Create | OpenFlags::Truncate,
            error);
        if (!out) {
            result.error = error ? error : make_error_code(ErrorCode::io_error);
            return result;
        }

        uint64_t written = 0;
        while (written < thisSize) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<uint64_t>(buffer.size(), thisSize - written));
            std::size_t got = 0;
            error = in->pread(offset + written, buffer.data(), want, got);
            if (error) {
                result.error = error;
                return result;
            }
            if (got == 0) {
                result.error = make_error_code(ErrorCode::truncated_metadata);
                return result;
            }

            std::size_t put = 0;
            error = out->pwrite(written, buffer.data(), got, put);
            if (error) {
                result.error = error;
                return result;
            }
            if (put != got) {
                result.error = make_error_code(ErrorCode::io_error);
                return result;
            }
            written += got;
        }

        result.parts.push_back(partPath);
        offset += thisSize;
    }

    return result;
}

} // namespace fragfs
