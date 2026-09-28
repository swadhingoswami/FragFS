#pragma once

#include <fragfs/mapper.h>
#include <fragfs/metadata.h>
#include <fragfs/posix_file.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>

namespace fragfs {

// The sidecar metadata path for a logical file: "combined.ff" -> "combined.ff.meta".
std::filesystem::path metadataPathFor(const std::filesystem::path& logicalPath);

// How many physical files a LogicalFile keeps open at once. Beyond this, the
// least recently used descriptor is closed.
inline constexpr std::size_t kDefaultMaxOpenFiles = 64;

// A logical file: metadata plus the ability to read the mapped bytes.
//
// It owns the mapping and an LRU cache of open physical files. Reads translate
// a logical range into physical reads via Mapper and execute them with pread().
// The cache is guarded by a mutex, and because pread() carries its own offset,
// concurrent reads from multiple threads are safe.
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
                                           std::error_code& error,
                                           std::size_t maxOpenFiles = kDefaultMaxOpenFiles);

    const Metadata& metadata() const { return metadata_; }
    uint64_t logicalSize() const { return metadata_.logicalSize; }
    const std::filesystem::path& baseDirectory() const { return baseDirectory_; }

    // Number of physical files currently held open. Intended for tests and
    // diagnostics.
    std::size_t openFileCount() const;

    // Reads up to `size` bytes starting at `logicalOffset` into `buffer`.
    // `bytesRead` receives the number of bytes actually available, which is
    // less than `size` only at end of file. Reads at or past end of file
    // succeed with zero bytes.
    std::error_code read(uint64_t logicalOffset,
                         void* buffer,
                         std::size_t size,
                         std::size_t& bytesRead);

private:
    struct OpenFile {
        std::shared_ptr<PosixFile> file;
        std::list<std::string>::iterator order;
    };

    LogicalFile(Metadata metadata, std::filesystem::path baseDirectory,
                std::size_t maxOpenFiles);

    // Returns an open descriptor for `storedPath`, opening it on first use and
    // refreshing its LRU position. The shared_ptr keeps the descriptor alive
    // even if another thread evicts it before the caller finishes its pread.
    std::error_code acquirePhysicalFile(const std::string& storedPath,
                                        std::shared_ptr<PosixFile>& file);

    void touch(const std::string& key);
    void evictIfNeeded();

    Metadata metadata_;
    std::filesystem::path baseDirectory_;
    std::size_t maxOpenFiles_;
    std::unique_ptr<std::mutex> cacheMutex_;
    std::unordered_map<std::string, OpenFile> openFiles_;
    std::list<std::string> lruOrder_; // front = most recently used
};

} // namespace fragfs
