# Design Decisions

This document records the *why* behind decisions that are otherwise surprising
or easy to accidentally reverse. Each entry states the decision, the reason,
and the alternatives that were rejected.

## D1 — Map instead of copy

**Decision:** A logical file stores only mappings; physical bytes are never
copied.

**Why:** Copying duplicates storage, costs time proportional to total size, and
can fail partway, leaving an inconsistent combined file. Mapping is O(number of
fragments) to create regardless of file size.

**Rejected:** Physical concatenation; reflink/hardlink tricks (not portable and
still require a copy for partial ranges).

## D2 — `pread()` instead of `lseek()` + `read()`

**Decision:** All logical reads translate to `pread()`.

**Why:** `lseek()` + `read()` mutates shared file-descriptor state. Two threads
reading through the same descriptor would race on the offset. `pread()` takes
the offset per call, so reads are naturally independent and thread-safe. This
is a prerequisite for the concurrency milestone.

**Rejected:** A mutex around `lseek()`/`read()`, which serialises readers and
still leaves the descriptor state global.

## D3 — `uint64_t` for offsets and lengths

**Decision:** All logical and physical offsets and lengths are `uint64_t`.

**Why:** Files can exceed 4 GiB. Signed types invite undefined behaviour on
overflow; `uint64_t` gives a predictable 2^64-byte address space and matches
`off_t` on 64-bit POSIX systems. Every arithmetic operation on offsets must
still be checked for overflow, because untrusted metadata can contain
adversarial values.

## D4 — Versioned, explicit metadata serialization

**Decision:** Metadata is serialized field-by-field into a documented byte
layout with a magic number and a format version. Raw `struct` dumps are
forbidden.

**Why:** Raw structs are not portable: they contain compiler-inserted padding,
differ in size across ABIs, and depend on host endianness. An explicit format
is stable across compilers and platforms and can evolve via the version field.

## D5 — Dependency-free test framework

**Decision:** Tests use a small in-tree header (`tests/test_framework.h`)
rather than Catch2/GoogleTest.

**Why:** The suite must build and run offline in CI with no package manager.
The framework is a few dozen lines and covers registration plus `CHECK`/`CHECK_EQ`.

**Rejected:** `FetchContent` of an external framework — adds a network
dependency and build complexity for functionality we barely use.

## D6 — Core is independent of any filesystem adapter

**Decision:** `fragfs_core` never includes FUSE or macOS filesystem headers.

**Why:** The mapping engine is the valuable, portable part of FragFS. Keeping
adapters out of the core means the same engine can be driven by the CLI, by
tests, by benchmarks, and later by a FUSE or macOS user-space filesystem layer
without modification.

## D7 — One POSIX implementation, not per-platform forks

**Decision:** A single POSIX implementation is used for Linux and macOS.

**Why:** The required primitives (`open`, `pread`, `fstat`, `fsync`, `rename`)
have identical semantics on both. Forking them would duplicate code and drift.
Genuinely platform-specific behaviour, if it appears, is confined to
`src/platform/`.

## D8 — Appending is O(1) in the number of existing bytes

**Decision:** `append` and `add` never read or rewrite the physical data. They
add exactly one fragment record to the metadata. The cost of appending file N
is independent of the total size of files 1..N-1.

**Why:** This is the entire point of FragFS. Re-reading the already-mapped
physical files and copying them forward would turn an O(1) metadata operation
into an O(total bytes) data operation, duplicate storage, and reintroduce the
failure mode we are trying to eliminate. `append` must therefore be a pure
metadata mutation.

**Consequence for the metadata file:** the metadata sidecar is rewritten on
each update (to keep crash-consistency via write-tmp + rename), but it is a
mapping table whose size is proportional to the *fragment count*, not to the
mapped data. Rewriting a few kilobytes of metadata is not "re-reading and
rewriting the file" in the sense that matters. If fragment counts ever grow
large enough that rewriting the table is measurable, an append-only metadata
log with periodic compaction is the documented upgrade path; the mapper
interface would not change.

**Rejected:** Copying physical data into a single growing file on append;
re-reading all prior fragments to rebuild a combined blob.

## D9 — Half-open ranges and explicit overflow checks

**Decision:** Fragment ranges are half-open (`[start, start + length)`), and
every end offset is computed through an overflow-checked helper that returns
`std::optional<uint64_t>` rather than wrapping.

**Why:** Half-open ranges make boundaries unambiguous — the byte at the end of
one fragment is the start of the next, and a zero-length fragment correctly
contains nothing. Overflow checking matters because metadata is untrusted: a
corrupted `length` can make `start + length` wrap to a small value, which would
turn a validation failure into an out-of-bounds read. `checkedAdd` converts that
into a detectable `nullopt`.

**Consequence:** `Fragment::logicalEnd()` / `physicalEnd()` are fallible, and
callers must handle the failure rather than assuming a valid range. Validation
is explicit (`hasValidRanges()`), not enforced by the type, so a fragment can
exist transiently in a partially-parsed state.
