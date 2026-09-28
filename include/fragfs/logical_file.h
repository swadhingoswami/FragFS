#pragma once

#include <fragfs/mapper.h>
#include <fragfs/metadata.h>
#include <fragfs/posix_file.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>

namespace fragfs {

// The sidecar metadata path for a logical file: "combined.ff" -> "combined.ff.meta".
std::filesystem::path metadataPathFor(const std::filesystem::path& logicalPath);

// A logical file: metadata plus the ability to read the mapped bytes.
//
// It owns the mapping and a cache of open physical files. Reads translate a
// logical range into physical reads via Mapper and execute them with pread(),
// so no shared file position is involved.
class LogicalFile {
public:
    // Loads and decodes metadata from `metadataPath`.
    //
    // Physical file paths stored in the metadata are resolved relative to the
    // metadata file's directory, so a logical file and its fragments can be
    // moved together. Opening does not touch the physical files; they are
    // opened lazily on first read.
    static std::optional<LogicalFile> open(const std::filesystem::path& metadataPath,
                                           std::error_code& error);

    const Metadata& metadata() const { return metadata_; }
    uint64_t logicalSize() const { return metadata_.logicalSize; }
    const std::filesystem::path& baseDirectory() const { return baseDirectory_; }

    // Reads up to `size` bytes starting at `logicalOffset` into `buffer`.
    // `bytesRead` receives the number of bytes actually available, which is
    // less than `size` only at end of file. Reads at or past end of file
    // succeed with zero bytes. This is not const because it may open and cache
    // physical files.
    std::error_code read(uint64_t logicalOffset,
                         void* buffer,
                         std::size_t size,
                         std::size_t& bytesRead);

private:
    LogicalFile(Metadata metadata, std::filesystem::path baseDirectory);

    std::filesystem::path resolvePhysicalPath(const std::string& storedPath) const;

    // Returns an open descriptor for `storedPath`, opening and caching it on
    // first use. The returned pointer stays valid for the lifetime of the
    // LogicalFile (unordered_map references are stable).
    std::error_code acquirePhysicalFile(const std::string& storedPath,
                                        PosixFile*& file);

    Metadata metadata_;
    std::filesystem::path baseDirectory_;
    std::unordered_map<std::string, PosixFile> openFiles_;
};

} // namespace fragfs
