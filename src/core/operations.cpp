#include <fragfs/operations.h>

#include <fragfs/checked_arithmetic.h>
#include <fragfs/error.h>
#include <fragfs/metadata_store.h>
#include <fragfs/posix_file.h>

#include <cstddef>
#include <utility>

namespace fragfs {
namespace {

// Stats a physical file and returns its size. Distinguishes a missing path, a
// non-regular path, and an unreadable file.
std::error_code statPhysicalFile(const std::filesystem::path& file, uint64_t& size) {
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
    return opened->size(size);
}

std::error_code validateResult(const Metadata& metadata) {
    return metadata.validate().error;
}

} // namespace

CreateResult buildMetadata(const std::vector<std::filesystem::path>& files,
                           const std::filesystem::path& metadataPath) {
    CreateResult result;
    const std::filesystem::path baseDirectory = baseDirectoryFor(metadataPath);

    uint64_t logicalStart = 0;
    for (const std::filesystem::path& file : files) {
        uint64_t size = 0;
        result.error = statPhysicalFile(file, size);
        if (result.error) {
            return result;
        }
        if (size == 0) {
            result.error = make_error_code(ErrorCode::empty_physical_file);
            return result;
        }

        const std::optional<uint64_t> logicalEnd = checkedAdd(logicalStart, size);
        if (!logicalEnd.has_value()) {
            result.error = make_error_code(ErrorCode::invalid_range);
            return result;
        }

        Fragment fragment;
        fragment.logicalStart = logicalStart;
        fragment.physicalStart = 0;
        fragment.length = size;
        fragment.path = storePath(file, baseDirectory).string();
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
    uint64_t size = 0;
    std::error_code error = statPhysicalFile(file, size);
    if (error) {
        return error;
    }
    if (size == 0) {
        return make_error_code(ErrorCode::empty_physical_file);
    }

    const std::optional<uint64_t> logicalEnd = checkedAdd(metadata.logicalSize, size);
    if (!logicalEnd.has_value()) {
        return make_error_code(ErrorCode::invalid_range);
    }

    Fragment fragment;
    fragment.logicalStart = metadata.logicalSize;
    fragment.physicalStart = 0;
    fragment.length = size;
    fragment.path = storePath(file, baseDirectoryFor(metadataPath)).string();
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
    std::error_code error = statPhysicalFile(file, fileSize);
    if (error) {
        return error;
    }

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

} // namespace fragfs
