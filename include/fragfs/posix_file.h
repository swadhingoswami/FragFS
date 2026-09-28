#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

namespace fragfs {

// Access and creation flags for opening a file. Kept independent of the POSIX
// <fcntl.h> constants so the rest of FragFS does not depend on platform
// headers; posix_file.cpp translates these.
enum class OpenFlags : unsigned {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1,
    Create = 1u << 2,
    Truncate = 1u << 3,
};

constexpr OpenFlags operator|(OpenFlags lhs, OpenFlags rhs) {
    return static_cast<OpenFlags>(static_cast<unsigned>(lhs) |
                                  static_cast<unsigned>(rhs));
}

constexpr bool hasFlag(OpenFlags set, OpenFlags flag) {
    return (static_cast<unsigned>(set) & static_cast<unsigned>(flag)) != 0;
}

// Move-only RAII wrapper around a POSIX file descriptor.
//
// All operations report failure through std::error_code rather than throwing,
// because a missing or malformed file is routine input for FragFS, not an
// exceptional condition. The destructor closes the descriptor; the class is
// move-only so a descriptor is never closed twice.
class PosixFile {
public:
    PosixFile() = default;
    ~PosixFile();

    PosixFile(PosixFile&& other) noexcept;
    PosixFile& operator=(PosixFile&& other) noexcept;
    PosixFile(const PosixFile&) = delete;
    PosixFile& operator=(const PosixFile&) = delete;

    // Opens `path`. On success returns the file and leaves `error` cleared; on
    // failure returns std::nullopt and sets `error` to the errno-derived code.
    static std::optional<PosixFile> open(const std::filesystem::path& path,
                                         OpenFlags flags,
                                         std::error_code& error);

    bool isOpen() const { return descriptor_ >= 0; }

    // The underlying descriptor, or -1. Intended for advanced/internal use.
    int nativeHandle() const { return descriptor_; }

    // Closes the descriptor. Idempotent: closing an already-closed file
    // succeeds. On failure the descriptor is still considered released.
    std::error_code close();

    // Current file size in bytes.
    std::error_code size(uint64_t& out) const;

    // Reads up to `size` bytes at `offset` (independent of any file position,
    // so it is safe under concurrent use). `bytesRead` receives the count; a
    // value below `size` means end of file was reached. Retries on EINTR.
    std::error_code pread(uint64_t offset,
                          void* buffer,
                          std::size_t size,
                          std::size_t& bytesRead) const;

    // Writes up to `size` bytes at `offset`. `bytesWritten` receives the
    // count. Retries on EINTR.
    std::error_code pwrite(uint64_t offset,
                           const void* buffer,
                           std::size_t size,
                           std::size_t& bytesWritten) const;

    // Flushes the file's data and metadata to stable storage.
    std::error_code sync();

private:
    explicit PosixFile(int descriptor) : descriptor_(descriptor) {}

    int descriptor_ = -1;
};

} // namespace fragfs
