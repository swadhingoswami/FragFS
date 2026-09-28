# Crash Consistency

FragFS updates metadata with a write-then-rename sequence, implemented in
`writeMetadataFile()` (`src/core/metadata_store.cpp`).

## The hazard

The naive update is unsafe:

```text
open(metadata)
truncate()        <-- a crash here loses all metadata
write()           <-- a crash here leaves a half-written file
```

A crash at either point leaves the logical file unusable.

## The strategy: write, fsync, rename, fsync

```text
1. write new metadata to   combined.ff.meta.tmp   (same directory)
2. fsync()                 the temporary file
3. rename()                combined.ff.meta.tmp -> combined.ff.meta
4. fsync()                 the containing directory
```

`rename()` within a directory is atomic on POSIX: a concurrent reader observes
either the old file or the new file, never a partial one. The two `fsync()`
calls are what turn "the rename happened" into "the rename is durable" — first
the file's contents, then the directory entry.

## Guarantees this provides

- A reader never sees partially written metadata; it sees one consistent
  version or the other.
- After a crash, the metadata on disk is either the previous consistent
  version or the new consistent version.
- If the process dies before the rename, the destination is untouched and only
  a stray `.tmp` file remains.

## What this does **not** guarantee

- The *contents* of physical files are not covered. FragFS never writes them.
- Durability of the rename depends on the filesystem honouring `fsync` on the
  directory. Where a platform rejects directory `fsync` (some filesystems
  return `EINVAL`/`ENOTSUP`), `syncDirectory()` treats it as best-effort rather
  than failing the operation.
- Concurrent *writers* are not serialised. Two simultaneous writers would race
  on the same `.tmp` path. Reader/writer and multi-reader safety are in scope;
  multi-writer coordination is not.
- The physical files are not snapshotted. If another process modifies a
  fragment after `create`, reads and `verify` will report the mismatch
  (`physical_range_out_of_bounds`); FragFS does not lock or copy the data.
