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

### Header (version 2, 36 bytes)

```text
offset  size  field
------  ----  -----
0       8     magic           ASCII "FRAGFSM1"
8       4     format_version  uint32 (currently 2)
12      4     flags           uint32 (reserved, 0)
16      8     logical_size    uint64
24      8     fragment_count  uint64
32      4     checksum        uint32 (CRC-32 of the fragment region)
```

Version 1 used a 32-byte header with no `checksum` field. The decoder still
reads version-1 files; the writer always emits version 2.

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

1. a buffer shorter than the 12 bytes needed for magic + version
   (`truncated_metadata`);
2. a wrong magic (`invalid_magic`);
3. an unknown `format_version` (`unsupported_version`);
4. a buffer shorter than the version's header (`truncated_metadata`);
5. `fragment_count > kMaxFragments` (`invalid_fragment_count`);
6. a `fragment_count` that cannot fit in the remaining buffer
   (`truncated_metadata`);
7. a truncated fragment record or a `path_length` past the end of the buffer
   (`truncated_metadata`);
8. for version 2, a fragment region whose CRC-32 does not match the header
   (`checksum_mismatch`);
9. structural invalidity of the decoded metadata — overlapping/gapped
   fragments, overflowing ranges, empty paths, or a `logical_size` that does
   not match (`overlapping_fragments`, `gap_between_fragments`,
   `invalid_range`, `empty_path`, `logical_size_mismatch`).

The checksum is verified *after* the bounds-checked decode, so a truncated file
is reported as truncation rather than as a checksum failure. The checksum covers
the fragment region only; header fields are constrained by the explicit checks
above (magic, version, count bounds) and by structural validation
(`logical_size` must equal the last fragment's end).

Trailing bytes after the last fragment are currently ignored. A future version
may use them for extension data; the `flags` field is reserved for the same
purpose.
