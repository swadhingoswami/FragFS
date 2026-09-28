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
  (magic, version, logical size).
- **Mapper** — pure function from `(logicalOffset, size)` to a sequence of
  `(fragment, physicalOffset, length)` read steps.
- **LogicalFile** — ties metadata, mapper, and POSIX I/O together to implement
  `read()`.

The core knows nothing about FUSE, the CLI, or how it is being driven.

### Platform (`src/platform`)

Thin wrappers over POSIX calls (`open`, `close`, `pread`, `fstat`, `fsync`,
`rename`). Behaviour is identical on Linux and macOS, so there is currently a
single POSIX implementation. Platform-specific code, when unavoidable, is
isolated here behind the same interface.

## Data flow of a read

```text
LogicalFile::read(logicalOffset, buffer, size)
        │
        ▼
Mapper::plan(logicalOffset, size)   ->  [ ReadStep, ReadStep, ... ]
        │
        ▼ for each step
POSIXFile::pread(physicalOffset, buffer, length)
        │
        ▼
     physical file
```

The mapper is intentionally a pure computation with no I/O, which makes the
offset-translation logic exhaustively unit-testable without touching a disk.

## Why this separation matters

- The mapping engine can be tested in isolation.
- A filesystem adapter can be added later without modifying the core.
- The CLI can be replaced by a library API without disturbing I/O.
