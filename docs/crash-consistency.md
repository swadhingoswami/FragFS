# Crash Consistency

> **Status:** design only. Implementation arrives in the crash-safe metadata
> milestone. Nothing in the current code modifies metadata.

## The hazard

The naive update is unsafe:

```text
open(metadata)
truncate()        <-- a crash here loses all metadata
write()           <-- a crash here leaves a half-written file
```

A crash at either point leaves the logical file unusable.

## Planned strategy: write-then-rename

```text
1. write new metadata to   combined.ff.meta.tmp
2. fsync()                 the temporary file
3. rename()                combined.ff.meta.tmp -> combined.ff.meta
4. fsync()                 the containing directory
```

`rename()` within a directory is atomic on POSIX: readers observe either the
old file or the new file, never a partial one. The `fsync()` calls are what
turn "the rename happened" into "the rename is durable".

## Guarantees this provides

- A reader never sees partially written metadata.
- After a crash, the metadata is either the previous consistent version or the
  new consistent version.

## What this does **not** guarantee

- The *contents* of the physical files are not covered. FragFS never writes to
  them.
- Power-loss durability of the rename itself depends on the filesystem honouring
  `fsync` on the directory; this is a POSIX/firmware assumption we document
  rather than paper over.
- Concurrent writers are not made safe by this scheme; writer serialisation is
  a separate concern.

These limits are stated explicitly so the documentation never claims a stronger
guarantee than the implementation provides.
