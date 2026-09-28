# Mapping

> **Status:** not yet implemented. This document will be finalised in the
> mapper milestone.

## The problem

A logical file is a contiguous byte range assembled from an ordered list of
fragments. A caller asks for `read(logicalOffset, size)`. That request may:

- fall entirely inside one fragment,
- start in one fragment and end in another,
- span many fragments,
- run past the logical end of file.

## Translation

For a request `[logicalOffset, logicalOffset + size)`:

1. Find the first fragment whose logical range contains `logicalOffset`.
2. Compute the fragment-relative offset:
   `relative = logicalOffset - fragment.logicalStart`.
3. Translate to the physical offset:
   `physicalOffset = fragment.physicalStart + relative`.
4. The maximum number of bytes readable from this fragment is:
   `fragment.length - relative`.
5. Read `min(remaining, available)` bytes and advance to the next fragment.

## Worked example

Fragments:

```text
F1 [0,100)      -> part1.dat
F2 [100,150)    -> part2.dat
F3 [150,350)    -> part3.dat
```

Request `read(80, 100)` becomes:

```text
F1: physical offset 80, read 20 bytes
F2: physical offset 0,  read 50 bytes
F3: physical offset 0,  read 30 bytes
```

## Lookup strategy

Fragments are stored ordered by `logicalStart` and are non-overlapping. The
initial implementation uses a linear scan, which is simple and correct. Because
the list is sorted, a later milestone can switch to binary search without
changing the mapper's interface.
