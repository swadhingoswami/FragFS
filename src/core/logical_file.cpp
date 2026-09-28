#include <fragfs/logical_file.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace fragfs {
namespace {

// Refuse to load an implausibly large metadata file rather than allocating
// memory proportional to it. Real metadata is kilobytes; anything near this
// bound indicates corruption or a hostile file.
constexpr uint64_t kMaxMetadataBytes = 256ull * 1024 * 1024;

std::error_code readWholeFile(const std::filesystem::path& path,
                              std::vector<std::byte>& out) {
    std::error_code error;
    auto file = PosixFile::open(path, OpenFlags::Read, error);
    if (!file) {
        return error ? error : make_error_code(ErrorCode::io_error);
    }

    uint64_t size = 0;
    error = file->size(size);
    if (error) {
        return error;
    }
    if (size > kMaxMetadataBytes) {
        return std::make_error_code(std::errc::file_too_large);
    }

    out.resize(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < out.size()) {
        std::size_t bytesRead = 0;
        error = file->pread(offset, out.data() + offset, out.size() - offset,
                            bytesRead);
        if (error) {
            return error;
        }
        if (bytesRead == 0) {
            // The file shrank between fstat and pread.
            return make_error_code(ErrorCode::truncated_metadata);
        }
        offset += bytesRead;
    }
    return {};
}

} // namespace

std::filesystem::path metadataPathFor(const std::filesystem::path& logicalPath) {
    return std::filesystem::path(logicalPath.string() + ".meta");
}

LogicalFile::LogicalFile(Metadata metadata, std::filesystem::path baseDirectory)
    : metadata_(std::move(metadata)), baseDirectory_(std::move(baseDirectory)) {}

std::optional<LogicalFile> LogicalFile::open(const std::filesystem::path& metadataPath,
                                             std::error_code& error) {
    error.clear();

    std::vector<std::byte> buffer;
    error = readWholeFile(metadataPath, buffer);
    if (error) {
        return std::nullopt;
    }

    DecodeResult decoded = deserializeMetadata(buffer.data(), buffer.size());
    if (!decoded.ok()) {
        error = decoded.error;
        return std::nullopt;
    }

    std::filesystem::path baseDirectory = metadataPath.parent_path();
    if (baseDirectory.empty()) {
        baseDirectory = ".";
    }

    return LogicalFile(std::move(decoded.metadata), std::move(baseDirectory));
}

std::filesystem::path LogicalFile::resolvePhysicalPath(const std::string& storedPath) const {
    const std::filesystem::path path(storedPath);
    if (path.is_absolute()) {
        return path;
    }
    return baseDirectory_ / path;
}

std::error_code LogicalFile::acquirePhysicalFile(const std::string& storedPath,
                                                 PosixFile*& file) {
    const std::filesystem::path resolved = resolvePhysicalPath(storedPath);
    const std::string key = resolved.string();

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
