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
- The physical files are not snapshotted. If another process modifies a
  fragment after `create`, reads and `verify` will report the mismatch
  (`physical_range_out_of_bounds`); FragFS does not lock or copy the data.

## Writer serialisation

Crash safety alone does not stop two writers from losing each other's updates:
both could load the same metadata, each append its own fragment, and the second
`rename` would overwrite the first's work. FragFS therefore serialises writers
with an advisory exclusive lock (`flock`) on a sibling `<metadata>.lock` file.

The lock is held across the whole read-modify-write cycle by
`updateMetadata()`, not just the final write, because the load is part of the
race. `create` takes the same lock around its single write. `flock` locks
belong to the open file description, so separate opens — including from
different threads of one process — exclude each other; this is why `flock` is
used rather than per-process `fcntl` locks.

The lock file is separate from the metadata file on purpose: the metadata is
replaced by `rename`, which would invalidate a lock held on the old inode.
