#pragma once

#include <cstdint>

namespace fragfs {

// Identity of a physical file as observed through fstat at the time a fragment
// was recorded. Used by verify to detect a fragment whose file has since been
// modified (size/mtime) or replaced (device/inode).
//
// These values are inherently platform-derived (st_dev, st_ino, st_mtime) but
// are stored as plain integers so the core model stays independent of POSIX
// headers.
struct FileIdentity {
    uint64_t device = 0;
    uint64_t inode = 0;
    uint64_t size = 0;
    int64_t mtimeSeconds = 0;
    uint32_t mtimeNanoseconds = 0;
};

inline bool operator==(const FileIdentity& lhs, const FileIdentity& rhs) {
    return lhs.device == rhs.device && lhs.inode == rhs.inode &&
           lhs.size == rhs.size && lhs.mtimeSeconds == rhs.mtimeSeconds &&
           lhs.mtimeNanoseconds == rhs.mtimeNanoseconds;
}

inline bool operator!=(const FileIdentity& lhs, const FileIdentity& rhs) {
    return !(lhs == rhs);
}

} // namespace fragfs
