# File Format

> **Status:** not yet implemented. This document will be finalised in the
> metadata-serialization milestone.

## Planned layout

The metadata sidecar (for example `combined.ff.meta`) will use an explicit,
versioned, little-endian byte layout:

```text
offset  size  field
------  ----  -----
0       8     magic          ("FRAGFS\0\0" / a fixed 8-byte constant)
8       4     format_version (uint32)
12      4     reserved / flags
16      8     logical_size   (uint64)
24      8     fragment_count (uint64)
32      ...   fragment records
```

Each fragment record:

```text
size  field
----  -----
8     logical_start  (uint64)
8     physical_start (uint64)
8     length         (uint64)
4     path_length    (uint32, bytes, not including terminator)
...   path           (UTF-8, no NUL terminator)
```

## Rules

- All integers are little-endian on disk, independent of host endianness.
- No C++ structs are written directly; padding and ABI differences are
  avoided by explicit field-by-field encoding.
- The version field gates parsing: an unknown version is rejected rather than
  guessed at.
- Every length is validated against the actual remaining file size before it
  is used to allocate memory or advance a cursor.
