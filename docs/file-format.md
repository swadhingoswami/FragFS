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

### Header

```text
offset  size  field
------  ----  -----
0       8     magic           ASCII "FRAGFSM1"
8       4     format_version  uint32 (currently 4)
12      4     flags           uint32 (reserved, 0)
16      8     logical_size    uint64
24      8     fragment_count  uint64
32      4     region_checksum uint32 (CRC-32 of the fragment region)
36      4     name_length     uint32 (v4 only)
40      ...   original_name   UTF-8 bytes (v4 only)
```

Version history:

- **v1** — 32-byte header, no region checksum, no identity, no name.
- **v2** — adds `region_checksum`.
- **v3** — adds per-fragment physical identity.
- **v4** — adds the original filename to the header and a per-fragment content
  CRC-32. Written by `split`; it is the manifest used by `get`.

The decoder reads all four versions. The writer emits **v4 when every fragment
carries a content checksum**, otherwise **v3 when every fragment carries
identity**, otherwise **v2**; it never emits v1.

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

Version 3 appends a 36-byte identity block to each record:

```text
size  field
----  -----
8     device          uint64
8     inode           uint64
8     size            uint64
8     mtime_sec       int64
4     mtime_nsec      uint32
```

Version 4 appends a 4-byte content checksum to each record:

```text
size  field
----  -----
4     crc32           uint32 (CRC-32 of the chunk's contents)
```

Each record's minimum size is 28 bytes (v1/v2), 64 bytes (v3), or 32 bytes
(v4). Paths are variable length, so records are not fixed-stride; the decoder
walks them sequentially. Identity lets `verify` detect a file modified or
replaced since creation; the v4 checksum detects a corrupted chunk during
reassembly.

## Validation on decode

The decoder rejects, in order:

1. a buffer shorter than the 12 bytes needed for magic + version
   (`truncated_metadata`);
2. a wrong magic (`invalid_magic`);
3. an unknown `format_version` (`unsupported_version`);
4. a buffer shorter than the version's header (including the v4 name)
   (`truncated_metadata`);
5. `fragment_count > kMaxFragments` (`invalid_fragment_count`);
6. a `fragment_count` that cannot fit in the remaining buffer
   (`truncated_metadata`);
7. a truncated fragment record or a `path_length` past the end of the buffer
   (`truncated_metadata`);
8. for version 2+, a fragment region whose CRC-32 does not match the header
   (`checksum_mismatch`);
9. structural invalidity of the decoded metadata — overlapping/gapped
   fragments, overflowing ranges, empty paths, or a `logical_size` that does
   not match (`overlapping_fragments`, `gap_between_fragments`,
   `invalid_range`, `empty_path`, `logical_size_mismatch`).

The region checksum is verified *after* the bounds-checked decode, so a
truncated file is reported as truncation rather than as a checksum failure. It
covers the fragment region only; header fields are constrained by the explicit
checks above and by structural validation.

Trailing bytes after the last fragment are currently ignored. A future version
may use them for extension data; the `flags` field is reserved for the same
purpose.
