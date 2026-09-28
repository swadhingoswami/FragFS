#include <fragfs/metadata_store.h>

#include <fragfs/posix_file.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fragfs {
namespace {

// Refuse to load an implausibly large metadata file rather than allocating
// memory proportional to it. Real metadata is kilobytes.
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
            return make_error_code(ErrorCode::truncated_metadata);
        }
        offset += bytesRead;
    }
    return {};
}

} // namespace

std::filesystem::path baseDirectoryFor(const std::filesystem::path& metadataPath) {
    std::filesystem::path base = metadataPath.parent_path();
    if (base.empty()) {
        base = ".";
    }
    return base;
}

std::filesystem::path storePath(const std::filesystem::path& file,
                                const std::filesystem::path& baseDirectory) {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(file, baseDirectory, error);
    if (!error && !relative.empty()) {
        return relative;
    }
    error.clear();
    const std::filesystem::path absolute = std::filesystem::absolute(file, error);
    if (!error) {
        return absolute;
    }
    return file;
}

std::filesystem::path resolvePhysicalPath(const std::string& storedPath,
                                          const std::filesystem::path& baseDirectory) {
    const std::filesystem::path path(storedPath);
    if (path.is_absolute()) {
        return path;
    }
    return baseDirectory / path;
}

std::error_code readMetadataFile(const std::filesystem::path& metadataPath,
                                 Metadata& out) {
    std::vector<std::byte> buffer;
    std::error_code error = readWholeFile(metadataPath, buffer);
    if (error) {
        return error;
    }

    DecodeResult decoded = deserializeMetadata(buffer.data(), buffer.size());
    if (!decoded.ok()) {
        return decoded.error;
    }
    out = std::move(decoded.metadata);
    return {};
}

std::error_code writeMetadataFile(const std::filesystem::path& metadataPath,
                                  const Metadata& metadata) {
    const std::vector<std::byte> bytes = serializeMetadata(metadata);

    // The temporary lives in the same directory as the destination so that the
    // rename stays within one filesystem and is therefore atomic.
    const std::filesystem::path temporary(metadataPath.string() + ".tmp");

    std::error_code error;
    {
        auto file = PosixFile::open(
            temporary, OpenFlags::Write | OpenFlags::Create | OpenFlags::Truncate,
            error);
        if (!file) {
            return error ? error : make_error_code(ErrorCode::io_error);
        }

        std::size_t offset = 0;
        while (offset < bytes.size()) {
            std::size_t written = 0;
            error = file->pwrite(offset, bytes.data() + offset,
                                 bytes.size() - offset, written);
            if (error) {
                return error;
            }
            if (written == 0) {
                return make_error_code(ErrorCode::io_error);
            }
            offset += written;
        }

        // Make the temporary's contents durable before the rename.
        error = file->sync();
        if (error) {
            return error;
        }
    }

    // Atomic replacement. A reader sees the old file or the new one.
    std::filesystem::rename(temporary, metadataPath, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return error;
    }

    // Make the rename itself durable.
    return syncDirectory(baseDirectoryFor(metadataPath));
}

std::optional<MetadataLock> MetadataLock::acquire(
    const std::filesystem::path& metadataPath, std::error_code& error) {
    error.clear();

    // The lock file is separate from the metadata file: the metadata is
    // replaced by rename, which would break a lock held on the old inode.
    const std::filesystem::path lockPath(metadataPath.string() + ".lock");

    auto file = PosixFile::open(
        lockPath, OpenFlags::Read | OpenFlags::Write | OpenFlags::Create, error);
    if (!file) {
        return std::nullopt;
    }
    error = file->lockExclusive();
    if (error) {
        return std::nullopt;
    }
    return MetadataLock(std::move(*file));
}

std::error_code updateMetadata(
    const std::filesystem::path& metadataPath,
    const std::function<std::error_code(Metadata&)>& mutate) {
    std::error_code error;
    std::optional<MetadataLock> lock = MetadataLock::acquire(metadataPath, error);
    if (!lock.has_value()) {
        return error;
    }

    Metadata metadata;
    error = readMetadataFile(metadataPath, metadata);
    if (error) {
        return error;
    }

    error = mutate(metadata);
    if (error) {
        return error;
    }

    return writeMetadataFile(metadataPath, metadata);
}

} // namespace fragfs
