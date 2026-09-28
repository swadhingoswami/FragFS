#pragma once

#include <fragfs/metadata.h>
#include <fragfs/posix_file.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <system_error>

namespace fragfs {

// The base directory used to resolve relative physical paths: the directory
// containing the metadata file (or "." when it has no parent component).
std::filesystem::path baseDirectoryFor(const std::filesystem::path& metadataPath);

// Converts a physical file path into the form stored in metadata: relative to
// `baseDirectory` when that is possible, otherwise absolute. Falling back to
// absolute keeps the mapping usable even when a relative path would be
// awkward or impossible.
std::filesystem::path storePath(const std::filesystem::path& file,
                                const std::filesystem::path& baseDirectory);

// Resolves a stored physical path against the base directory: absolute paths
// are returned unchanged, relative paths are joined to `baseDirectory`.
std::filesystem::path resolvePhysicalPath(const std::string& storedPath,
                                          const std::filesystem::path& baseDirectory);

// Reads and decodes a metadata sidecar. On success `out` holds the decoded
// metadata; on failure `out` is unspecified.
std::error_code readMetadataFile(const std::filesystem::path& metadataPath,
                                 Metadata& out);

// Serializes and writes metadata crash-safely:
//   1. write to a temporary file in the same directory,
//   2. fsync the temporary file,
//   3. atomically rename it over the destination,
//   4. fsync the directory.
// A reader therefore sees either the previous consistent metadata or the new
// consistent metadata, never a half-written file.
std::error_code writeMetadataFile(const std::filesystem::path& metadataPath,
                                  const Metadata& metadata);

// An advisory exclusive lock over a metadata file, used to serialise writers.
// The lock lives in a sibling "<metadata>.lock" file, so it is independent of
// the atomic rename of the metadata itself. The lock is released when the
// object is destroyed.
class MetadataLock {
public:
    MetadataLock(MetadataLock&&) noexcept = default;
    MetadataLock& operator=(MetadataLock&&) noexcept = default;
    MetadataLock(const MetadataLock&) = delete;
    MetadataLock& operator=(const MetadataLock&) = delete;

    // Blocks until the lock is held, or returns std::nullopt with `error` set.
    static std::optional<MetadataLock> acquire(const std::filesystem::path& metadataPath,
                                               std::error_code& error);

private:
    explicit MetadataLock(PosixFile file) : file_(std::move(file)) {}

    PosixFile file_;
};

// Performs a locked read-modify-write: acquires the writer lock, loads the
// metadata, applies `mutate`, and writes the result back atomically. Because
// the lock covers the load as well as the store, concurrent updates cannot
// lose each other. On error the metadata on disk is left unchanged.
std::error_code updateMetadata(const std::filesystem::path& metadataPath,
                               const std::function<std::error_code(Metadata&)>& mutate);

} // namespace fragfs
