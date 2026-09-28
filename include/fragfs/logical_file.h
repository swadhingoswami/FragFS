#pragma once

#include <fragfs/mapper.h>
#include <fragfs/metadata.h>
#include <fragfs/posix_file.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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
// logical range into physical reads via Mapper and execute them with pread().
// The descriptor cache is guarded by a mutex, and because pread() carries its
// own offset, concurrent reads from multiple threads are safe.
class LogicalFile {
public:
    LogicalFile(LogicalFile&&) noexcept;
    LogicalFile& operator=(LogicalFile&&) noexcept;
    LogicalFile(const LogicalFile&) = delete;
    LogicalFile& operator=(const LogicalFile&) = delete;
    ~LogicalFile();

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
    // succeed with zero bytes.
    std::error_code read(uint64_t logicalOffset,
                         void* buffer,
                         std::size_t size,
                         std::size_t& bytesRead);

private:
    LogicalFile(Metadata metadata, std::filesystem::path baseDirectory);

    // Returns an open descriptor for `storedPath`, opening and caching it on
    // first use. The returned pointer stays valid for the lifetime of the
    // LogicalFile (unordered_map references are stable). Thread-safe.
    std::error_code acquirePhysicalFile(const std::string& storedPath,
                                        PosixFile*& file);

    Metadata metadata_;
    std::filesystem::path baseDirectory_;
    std::unique_ptr<std::mutex> cacheMutex_;
    std::unordered_map<std::string, PosixFile> openFiles_;
};

} // namespace fragfs
