# Filesystem Adapter Investigation

> **Status: design only.** No adapter is implemented. This document records the
> plan and the platform constraints so the work can start from a clear design.

## Goal

Expose a logical file through the operating system's filesystem interface so
ordinary programs can open and read it without knowing about FragFS:

```bash
fragfs mount combined.ff ./mnt
cat ./mnt/combined
```

The adapter is a *thin* translation layer. All mapping, validation, and I/O
already exist in the core; the adapter only has to answer filesystem callbacks
by delegating to `LogicalFile`.

## Why the core is adapter-free

`fragfs_core` has no dependency on FUSE, macFUSE, or FSKit. The adapter depends
on the core, never the other way around:

```text
Application
     │
     ▼
Filesystem Adapter        (future: src/adapters/...)
     │
     ▼
FragFS Core               (LogicalFile, Mapper, Metadata)
     │
     ▼
POSIX File Layer          (PosixFile)
     │
 ┌───┼────┐
 ▼   ▼    ▼
F1  F2   F3
```

This is why the core is testable and portable today, and why the adapter can be
added later without touching mapping logic.

## Linux: FUSE

FUSE (libfuse) lets a user-space process implement filesystem operations. The
two API levels matter:

- **High-level API** (`fuse_operations`, `fuse_main`): you implement
  `getattr`, `read`, `readdir`, etc. on paths. Simpler to write.
- **Low-level API** (`fuse_lowlevel_ops`): you implement operations on inode
  numbers. More control, better for performance, more bookkeeping.

For FragFS the high-level API is sufficient initially. The essential callbacks:

- `getattr(path)` — report the logical file as a regular file of
  `LogicalFile::logicalSize()` bytes.
- `read(path, buffer, size, offset)` — call
  `LogicalFile::read(offset, buffer, size, bytesRead)` and return the count.
- `open`/`release` — hold a `LogicalFile` per open handle.
- `readdir` — list the single logical entry.

Sketch of the read callback:

```cpp
static int fragfs_read(const char* path, char* buffer, size_t size,
                       off_t offset, struct fuse_file_info* info) {
    auto* file = static_cast<LogicalFile*>(fuse_get_context()->private_data);
    std::size_t bytesRead = 0;
    std::error_code error = file->read(static_cast<uint64_t>(offset), buffer,
                                       size, bytesRead);
    if (error) {
        return -EIO;
    }
    return static_cast<int>(bytesRead);
}
```

Note that `read` already has exactly the right shape — offset in, bytes out —
because the core was designed with this adapter in mind.

## macOS: not FUSE-by-copy

macOS has no in-tree FUSE. Historically the option was **macFUSE**, a
third-party kernel extension, which requires a kernel extension and a system
extension approval flow and does not ship with the OS. Two paths exist:

1. **macFUSE** — reuse the FUSE model via the third-party project. Pragmatic,
   but adds a heavy external dependency and an install step for users.
2. **FSKit** (macOS 15+) — Apple's supported user-space filesystem framework
   for extensions. This is the modern, first-party mechanism, but its API is
   *not* the FUSE API; the adapter would be written against FSKit's module
   protocol.

A third, lower-fidelity fallback is an **NFS loopback** server: implement a
tiny NFS server backed by the core and mount it via `mount_nfs` on localhost.
This avoids kernel extensions entirely at the cost of NFS semantics.

The Linux FUSE adapter must therefore **not** be assumed portable. The shared
part is the core; the adapter is platform-specific and lives outside it.

## Proposed layout

```text
src/adapters/
├── fuse/
│   └── fuse_adapter.cpp     # Linux, libfuse3
└── fskit/
    └── fskit_adapter.cpp    // macOS 15+, FSKit
```

CMake would gate each on the platform and on the presence of the SDK/library
(`find_package(PkgConfig)` + `pkg_check_modules(FUSE3 ...)` for Linux).

## Open questions to resolve before implementing

- Read-only vs. writable mounts. FragFS maps existing files; writes would need
  a defined semantics (append to a new fragment? copy-on-write?) that the core
  does not yet have.
- Handle lifetime and concurrency: one `LogicalFile` per mount or per open
  file descriptor? The core's mutex-guarded cache supports sharing.
- Reporting errors faithfully: map `std::error_code` values to the filesystem
  layer's errno-style return codes.
- macOS mechanism choice (FSKit vs. macFUSE vs. NFS) — this is the main
  unresolved platform decision.
