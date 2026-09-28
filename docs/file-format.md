# File Format

FragFS stores the mapping for a logical file in a sidecar metadata file (for
example `combined.ff.meta`). The metadata describes *where* the bytes live; it
never contains the bytes themselves.

## Design rules

- **Explicit, field-by-field encoding.** No C++ struct is written directly, so
  compiler padding, ABI differences, and struct layout changes cannot corrupt
  the format.
- **Little-endian on disk**, regardless of host endianness. The encoder and
  decoder convert explicitly byte by byte.
- **Versioned.** The format version gates parsing; an unknown version is
  rejected, never guessed at.
- **Untrusted input.** The decoder validates the magic, version, fragment
  count, and every length against the remaining buffer size *before* using it
  to allocate or advance, so a malformed file cannot cause an out-of-bounds
  read or an unbounded allocation.

## Layout

All integers are unsigned little-endian.

### Header (32 bytes)

```text
offset  size  field
------  ----  -----
0       8     magic           ASCII "FRAGFSM1"
8       4     format_version  uint32 (currently 1)
12      4     flags           uint32 (reserved, 0 in version 1)
16      8     logical_size    uint64
24      8     fragment_count  uint64
```

### Fragment records (repeated `fragment_count` times)

```text
size  field
----  -----
8     logical_start   uint64
8     physical_start  uint64
8     length          uint64
4     path_length     uint32 (bytes, excluding any terminator)
...   path            UTF-8 bytes, no NUL terminator
```

Each record's minimum size is 28 bytes. Paths are variable length, so records
are not fixed-stride; the decoder walks them sequentially.

## Validation on decode

The decoder rejects, in order:

1. a buffer shorter than the 32-byte header (`truncated_metadata`);
2. a wrong magic (`invalid_magic`);
3. an unknown `format_version` (`unsupported_version`);
4. `fragment_count > kMaxFragments` (`invalid_fragment_count`);
5. a `fragment_count` that cannot fit in the remaining buffer
   (`truncated_metadata`);
6. a truncated fragment record or a `path_length` past the end of the buffer
   (`truncated_metadata`);
7. structural invalidity of the decoded metadata — overlapping/gapped
   fragments, overflowing ranges, empty paths, or a `logical_size` that does
   not match (`overlapping_fragments`, `gap_between_fragments`,
   `invalid_range`, `empty_path`, `logical_size_mismatch`).

Trailing bytes after the last fragment are currently ignored. A future version
may use them for extension data or an integrity checksum; the `flags` field is
reserved for the same purpose.
