# Mapping

The mapper translates a logical read into a sequence of physical reads. It is
implemented in `src/core/mapper.cpp` and exposed through
`include/fragfs/mapper.h`.

## The problem

A logical file is a contiguous byte range assembled from an ordered list of
fragments. A caller asks for `read(logicalOffset, size)`. That request may fall
entirely inside one fragment, start in one fragment and end in another, span
many fragments, or run past the logical end of file.

## The mapper is pure

`Mapper::plan(logicalOffset, size)` performs **no I/O**. It returns a `ReadPlan`:

```cpp
struct ReadStep {
    std::size_t fragmentIndex;
    uint64_t physicalOffset;
    uint64_t length;
};

struct ReadPlan {
    std::error_code error;
    std::vector<ReadStep> steps;
    uint64_t bytesPlanned;
};
```

Keeping planning separate from I/O means every boundary condition can be tested
without touching a disk, and the same plan can be executed by any backend
(POSIX `pread`, an in-memory buffer, or a test double).

## Translation

For a request `[logicalOffset, logicalOffset + size)`:

1. If `size == 0` or `logicalOffset >= logicalSize`, return an empty plan.
   This mirrors `pread()`, which returns 0 rather than an error at end of file.
2. Clamp the request: `remaining = min(size, logicalSize - logicalOffset)`.
   Computing `logicalSize - logicalOffset` is safe because `logicalOffset <
   logicalSize`, and it avoids ever evaluating `logicalOffset + size`, which
   could overflow `uint64_t` for an adversarial size.
3. Find the fragment containing `logicalOffset` (linear scan).
4. For each fragment from there:
   - `relative = current - fragment.logicalStart`
   - `physicalOffset = fragment.physicalStart + relative`
   - `chunk = min(remaining, fragment.length - relative)`
   - emit a step, advance `current` by `chunk`, decrement `remaining`
5. Stop when `remaining == 0` or the fragments run out.

## Worked example

Fragments:

```text
F1 [0,100)   -> physical 1000
F2 [100,150) -> physical 0
F3 [150,350) -> physical 5000
```

`plan(80, 40)` becomes:

```text
step 0: fragment 0, physicalOffset 1080, length 20
step 1: fragment 1, physicalOffset 0,    length 20
bytesPlanned = 40
```

## Lookup strategy

Fragments are stored ordered by `logicalStart` and tile the logical space, so
the linear scan can be replaced by a binary search without changing the
interface. The initial implementation uses the linear scan for clarity.
