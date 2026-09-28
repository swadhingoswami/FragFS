#include <fragfs/logical_file.h>

#include <fragfs/metadata_store.h>

#include <cstddef>
#include <mutex>
#include <utility>

namespace fragfs {

std::filesystem::path metadataPathFor(const std::filesystem::path& logicalPath) {
    return std::filesystem::path(logicalPath.string() + ".meta");
}

LogicalFile::LogicalFile(Metadata metadata, std::filesystem::path baseDirectory)
    : metadata_(std::move(metadata)),
      baseDirectory_(std::move(baseDirectory)),
      cacheMutex_(std::make_unique<std::mutex>()) {}

LogicalFile::LogicalFile(LogicalFile&&) noexcept = default;
LogicalFile& LogicalFile::operator=(LogicalFile&&) noexcept = default;
LogicalFile::~LogicalFile() = default;

std::optional<LogicalFile> LogicalFile::open(const std::filesystem::path& metadataPath,
                                             std::error_code& error) {
    error.clear();

    Metadata metadata;
    error = readMetadataFile(metadataPath, metadata);
    if (error) {
        return std::nullopt;
    }

    std::filesystem::path baseDirectory = metadataPath.parent_path();
    if (baseDirectory.empty()) {
        baseDirectory = ".";
    }

    return LogicalFile(std::move(metadata), std::move(baseDirectory));
}

std::error_code LogicalFile::acquirePhysicalFile(const std::string& storedPath,
                                                 PosixFile*& file) {
    const std::filesystem::path resolved =
        resolvePhysicalPath(storedPath, baseDirectory_);
    const std::string key = resolved.string();

    const std::lock_guard<std::mutex> lock(*cacheMutex_);

    const auto existing = openFiles_.find(key);
    if (existing != openFiles_.end()) {
        file = &existing->second;
        return {};
    }

    std::error_code error;
    auto opened = PosixFile::open(resolved, OpenFlags::Read, error);
    if (!opened) {
        if (error == std::errc::no_such_file_or_directory) {
            return make_error_code(ErrorCode::missing_physical_file);
        }
        return error ? error : make_error_code(ErrorCode::io_error);
    }

    const auto inserted = openFiles_.emplace(key, std::move(*opened));
    file = &inserted.first->second;
    return {};
}

std::error_code LogicalFile::read(uint64_t logicalOffset,
                                  void* buffer,
                                  std::size_t size,
                                  std::size_t& bytesRead) {
    bytesRead = 0;

    if (buffer == nullptr && size > 0) {
        return std::make_error_code(std::errc::invalid_argument);
    }

    const ReadPlan plan = Mapper(metadata_).plan(logicalOffset, size);
    if (!plan.ok()) {
        return plan.error;
    }

    auto* destination = static_cast<std::byte*>(buffer);
    std::size_t total = 0;

    for (const ReadStep& step : plan.steps) {
        const Fragment& fragment = metadata_.fragments[step.fragmentIndex];

        PosixFile* physical = nullptr;
        std::error_code error = acquirePhysicalFile(fragment.path, physical);
        if (error) {
            return error;
        }

        std::size_t bytesReadThisStep = 0;
        error = physical->pread(step.physicalOffset, destination + total,
                                static_cast<std::size_t>(step.length),
                                bytesReadThisStep);
        if (error) {
            return error;
        }
        if (bytesReadThisStep != step.length) {
            // The physical file is shorter than the metadata claims. FragFS
            // never silently returns fewer bytes than the mapping promises.
            return make_error_code(ErrorCode::physical_range_out_of_bounds);
        }
        total += bytesReadThisStep;
    }

    bytesRead = total;
    return {};
}

} // namespace fragfs
