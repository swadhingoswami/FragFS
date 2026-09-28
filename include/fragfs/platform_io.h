#pragma once

#include <cstdint>
#include <system_error>

namespace fragfs {

// Copies `length` bytes from `srcFd` at `srcOffset` into `destFd` at
// `destOffset` without moving the data: a copy-on-write range clone (reflink).
//
// Returns std::errc::not_supported (or operation_not_supported) when the
// platform or filesystem does not implement range cloning; the caller then
// falls back to copyRange().
std::error_code cloneRange(int destFd,
                           uint64_t destOffset,
                           int srcFd,
                           uint64_t srcOffset,
                           uint64_t length);

// Copies `length` bytes from `srcFd` into `destFd` using the fastest available
// kernel path (copy_file_range on Linux, which can offload to the filesystem
// or a server), falling back to a pread/pwrite loop. Copies the whole length or
// returns an error. `bytesCopied` receives the number of bytes written.
std::error_code copyRange(int destFd,
                          uint64_t destOffset,
                          int srcFd,
                          uint64_t srcOffset,
                          uint64_t length,
                          uint64_t& bytesCopied);

} // namespace fragfs
