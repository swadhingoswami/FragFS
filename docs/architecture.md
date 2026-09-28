# Architecture

## Layers

FragFS is organised as a strict dependency stack. Each layer may depend only on
the layers below it.

```text
              ┌───────────────┐
              │      CLI      │   src/cli
              └───────┬───────┘
                      │
              ┌───────▼───────┐
              │   FragFS Core │   src/core, include/fragfs
              │               │
              │ Metadata      │
              │ Mapper        │
              │ LogicalFile   │
              └───────┬───────┘
                      │
              ┌───────▼───────┐
              │   POSIX I/O   │   src/platform
              └───────┬───────┘
                      │
                Physical Files
```

## Layer responsibilities

### CLI (`src/cli`)

Argument parsing and human-facing output only. It owns no mapping or I/O
logic; it translates a command line into calls on the core and formats the
result. This keeps the core reusable by future adapters (FUSE, tests,
benchmarks).

### Core (`src/core`, `include/fragfs`)

The heart of the system:

- **Fragment** — one contiguous mapping from a logical range to a physical
  file range.
- **Metadata** — the ordered list of fragments plus global properties
  (magic, version, logical size), with structural validation.
- **Serialization** — the versioned, little-endian on-disk encoding.
- **Mapper** — pure function from `(logicalOffset, size)` to a sequence of
  `(fragment, physicalOffset, length)` read steps.
- **LogicalFile** — ties metadata, mapper, and POSIX I/O together to implement
  `read()`, with a lazily-populated, mutex-guarded descriptor cache.
- **Metadata store** — reads/writes the sidecar; writes are crash-safe
  (write tmp, fsync, rename, fsync directory).
- **Operations** — create/append/add/remove, which mutate metadata only.
- **Verify** — checks metadata structure and every fragment's physical range.

The core knows nothing about FUSE, the CLI, or how it is being driven.

### Platform (`src/platform`)

Thin wrappers over POSIX calls (`open`, `close`, `pread`, `pwrite`, `fstat`,
`fsync`). Behaviour is identical on Linux and macOS, so there is currently a
single POSIX implementation. Platform-specific code, when unavoidable, is
isolated here behind the same interface.

`PosixFile` (in `include/fragfs/posix_file.h`) is a move-only RAII handle: it
owns the descriptor, closes it on destruction, and reports failures as
`std::error_code` mapped from `errno`. It exposes offset-based `pread`/`pwrite`
so there is no shared file-position state, which is what allows concurrent
reads later. The rest of FragFS never sees a raw descriptor or a POSIX header.

## Data flow of a read

`LogicalFile` ties the pieces together:

```text
LogicalFile::read(logicalOffset, buffer, size)
        │
        ▼
Mapper::plan(logicalOffset, size)          (pure)
        │
        ▼  ReadPlan = [ ReadStep, ReadStep, ... ]
        │
        ▼  for each step
acquirePhysicalFile(fragment.path)          (lazy open + cache)
        │
        ▼
PosixFile::pread(physicalOffset, buffer, length)
        │
        ▼
     physical file
```

The mapper is intentionally a pure computation with no I/O, which makes the
offset-translation logic exhaustively unit-testable without touching a disk. A
physical file shorter than its mapping claims is reported as
`physical_range_out_of_bounds` rather than silently returning a short read.

## Why this separation matters

- The mapping engine can be tested in isolation.
- A filesystem adapter can be added later without modifying the core.
- The CLI can be replaced by a library API without disturbing I/O.
