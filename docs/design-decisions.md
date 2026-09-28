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

## D10 — Fragments must tile the logical space exactly

**Decision:** Valid metadata requires the fragments to be ordered and to cover
`[0, logicalSize)` exactly: the first starts at 0, each starts where the
previous ended, and `logicalSize` equals the end of the last fragment. Gaps and
overlaps are rejected.

**Why:** Tiling guarantees that every logical offset maps to exactly one
fragment. With a gap, a read would land in unmapped space with no defined
result; with an overlap, two fragments would claim the same logical byte and
the mapping would be ambiguous. Enforcing a single invariant removes both
classes of undefined behaviour up front.

**Consequence:** zero-length fragments are rejected at the metadata level, even
though the `Fragment` type itself tolerates them. An "empty logical file" is
represented by zero fragments with `logicalSize == 0`, not by an empty fragment.

## D11 — Errors are reported as `std::error_code`

**Decision:** FragFS defines an `ErrorCode` enum registered as a
`std::error_code` category (`"fragfs"`). Validation returns a `ValidationResult`
that carries the error code, the offending fragment index, and a human-readable
detail string.

**Why:** `std::error_code` is the standard, lightweight, non-throwing mechanism
for reporting failure and composes with `std::system_error` and
`system_category()` (which will represent `errno` from POSIX calls later). A
bare `bool`/`int` would lose the reason; exceptions for expected, routine
failures (a corrupt file is routine input) would be the wrong tool. The extra
index/detail fields preserve the diagnostic context that `error_code` alone
cannot hold.

**Consequence:** callers test `!ec` for success and compare against specific
codes. Note that a default-constructed `std::error_code` is `system:0`, which is
falsy but *not* `==` to `fragfs:0`, because `operator==` compares the category
as well as the value.

## D12 — Fixed little-endian encoding, bounds-checked decode

**Decision:** Serialization writes each field explicitly in little-endian order
via `appendU32`/`appendU64`; deserialization reads with `readU32`/`readU64` and
validates every length against the remaining buffer before use. Raw `memcpy` of
structs is forbidden.

**Why:** Direct struct dumps would bake in padding, host endianness, and ABI
layout — a metadata file written by one compiler/platform could be misread by
another. Explicit encoding makes the format a stable contract. On the read side,
the buffer is untrusted: a `path_length` or `fragment_count` taken on faith
could cause an out-of-bounds read or an unbounded `reserve`. The decoder
therefore treats every field as suspect and checks it against the bytes that
actually remain.

**Consequence:** the decoder's fast "can the buffer hold this many fragments"
check is only a lower bound; because paths are variable length, an authoritative
per-fragment bounds check runs inside the loop. `serializeMetadata` is a pure
encoder with a validity precondition — validation is the caller's job (and is
also re-run inside `deserializeMetadata`, so decoded metadata is always
structurally sound).

## D13 — Planning is separated from I/O

**Decision:** `Mapper::plan()` is a pure function that returns a `ReadPlan` (a
list of `ReadStep`s) and performs no I/O. A later `LogicalFile` executes the
plan with `pread()`.

**Why:** Offset translation is where the subtle bugs live (boundaries, gaps,
overflow, EOF). Making it a pure computation means those cases are exhaustively
unit-testable with no files, no file descriptors, and no flakiness. It also lets
different backends execute the same plan and keeps the mapper free of any
platform dependency.

**Consequence:** reads are two-phase — plan, then execute. The plan for a
request that overruns EOF is shorter than the request; `bytesPlanned` reports
how many logical bytes are actually available, which the caller needs to return
a correct short-read count.

## D14 — EOF follows the `pread()` convention

**Decision:** a read at or past `logicalSize` yields an empty plan (zero bytes),
not an error; a read starting before EOF but overrunning is clamped to
`logicalSize`.

**Why:** This matches POSIX `pread()`, where reading at or past end of file
returns 0. Aligning with the underlying convention means the logical read API
composes naturally with code that expects `read`-like semantics, and avoids
inventing a second set of edge-case rules.

**Consequence:** the mapper never evaluates `logicalOffset + size`, which could
overflow; it works from `logicalSize - logicalOffset` instead. Callers that want
to distinguish a short read from a full read compare `bytesPlanned` to the
requested size.

## D15 — RAII descriptors with non-throwing, error-code factories

**Decision:** `PosixFile` owns a file descriptor via RAII (move-only; the
destructor closes it). It is created through a static `open()` that returns
`std::optional<PosixFile>` and reports failure via an `std::error_code`
out-parameter. `errno` is mapped through `std::generic_category()`.

**Why:** RAII guarantees the descriptor is released on every path, including
early returns and exceptions, which is the difference between a long-running
daemon that leaks descriptors and one that does not. A move-only type makes
double-close impossible. Non-throwing construction suits FragFS because a
missing or unreadable file is expected input, not an exceptional condition, and
it keeps the CLI in control of how errors are reported to the user.

**Consequence:** every call can fail and must be checked; there is no implicit
"open throws" path. `close()` is idempotent and its destructor ignores the
result (a destructor cannot report errors). `pread`/`pwrite` retry on `EINTR`,
guard `off_t`/`SSIZE_MAX` limits, and never use shared file-position state, so
they are safe to call concurrently on the same descriptor.

## D16 — Retry `EINTR` on I/O, but never on `close`

**Decision:** `pread`, `pwrite`, and `fsync` retry when interrupted by a signal
(`EINTR`). `close` is called once and its error is not retried.

**Why:** A signal can interrupt a blocking read or write before it transfers
any data; retrying is safe and necessary for correctness. `close`, however, is
special: on Linux the descriptor is released even when `close` reports `EINTR`,
so retrying can close an unrelated descriptor that the process opened in the
meantime — a classic file-descriptor race. Treating `close` as
non-retryable is the safe choice.

**Consequence:** callers see `EINTR`-free reads/writes but must treat a failed
`close` as "the descriptor is gone", not "try again".

## D17 — Metadata sidecar naming and relative-path resolution

**Decision:** the metadata for a logical file `combined.ff` lives at
`combined.ff.meta` (the logical path plus `.meta`). Physical file paths stored
in the metadata are resolved relative to the directory containing the metadata
file; absolute paths are used as-is.

**Why:** keeping the mapping in a sidecar means the logical file name stays
clean and the format is unambiguous (the logical file is not a container). A
fixed, derived name means every command finds the metadata without a separate
argument. Resolving relative paths against the metadata's directory makes a
logical file and its fragments relocatable *as a unit*: move the directory and
the mapping still works, which would not be true if paths were resolved against
the process's current working directory.

**Consequence:** create-time code (a later milestone) is responsible for
choosing what to store (relative where possible). A relative path with `..` can
escape the base directory; that is permitted, and verification (a later
milestone) will surface missing or moved files explicitly rather than silently.

## D18 — Physical files are opened lazily and cached

**Decision:** `LogicalFile` does not open physical files at load time. It opens
each distinct file on first use and caches one descriptor per resolved path for
the lifetime of the object.

**Why:** opening every fragment eagerly would fail on a logical file whose
fragments are not all present, even when the caller only reads a range that is
present, and it would consume one descriptor per fragment (a logical file with
thousands of fragments could exhaust `RLIMIT_NOFILE`). Lazy opening also means
an unreferenced fragment costs nothing.

**Consequence:** a missing physical file is reported at read time
(`missing_physical_file`), not at open time. Because `pread` carries its own
offset and a shared descriptor has no mutable position, the cache is safe to
share across concurrent readers without locking; eviction (an LRU) is future
work if descriptor pressure becomes a concern.

## D19 — Crash-safe metadata replacement

**Decision:** metadata is never modified in place. A new copy is written to a
temporary file in the same directory, `fsync`ed, atomically `rename`d over the
destination, and the directory is `fsync`ed.

**Why:** an in-place `truncate` + `write` leaves a window where a crash
destroys the mapping entirely. `rename` within a directory is atomic on POSIX,
so readers see the old metadata or the new metadata, never a mix. The `fsync`
calls are what make the new version durable rather than merely visible.

**Consequence:** updates cost one extra file write and two `fsync`s. The
temporary path is derived (`<metadata>.tmp`), so concurrent writers would
collide; writer serialisation is out of scope. Directory `fsync` is
best-effort on platforms/filesystems that reject it. See
`docs/crash-consistency.md`.

## D20 — Physical file identity is path plus a size check

**Decision:** fragments reference physical files by path only. The initial
implementation does not store device/inode, mtime, or a content hash. Instead,
`verify` (and `read`) check that the mapped physical range fits inside the
file's current size.

**Why:** paths are portable and human-readable, and the common failure mode —
a fragment being deleted or truncated — is caught by the size check at read
time (`physical_range_out_of_bounds`) and surfaced explicitly by `verify`. Storing
an inode would detect replacement, but inodes are not portable across
filesystems, are reused after deletion, and change under ordinary copy/restore
operations, which would produce false alarms.

**Consequence:** a file replaced by a different file of the same-or-larger size
is not detected; the mapping silently follows the new bytes. Stronger identity
(device+inode, size, mtime, or a checksum) is a documented future enhancement,
traded against portability and cost.

## D21 — Concurrent reads without locks around the read path

**Decision:** `LogicalFile` guards only its descriptor cache with a mutex.
The actual `pread` calls run without any lock.

**Why:** `pread` carries its own offset, so it has no shared mutable state to
race on — two threads can read through the same descriptor simultaneously.
Locking the whole read would serialise independent readers for no reason. The
one genuinely shared mutable structure is the open-file cache (a map that may
grow on first use), so that alone is locked.

**Consequence:** readers scale with the underlying device, not with a lock.
The returned descriptor pointer is stable (node-based container), so it stays
valid after the lock is released. Writer coordination remains out of scope.

## D22 — Binary-search fragment lookup

**Decision:** `Mapper::plan` locates the first fragment with
`std::upper_bound` over the ordered `logicalStart` values rather than a linear
scan.

**Why:** fragments are stored ordered and non-overlapping, so the containing
fragment can be found in O(log n). A logical file with many fragments (a
split archive, a log rotation) would otherwise pay O(n) on every read, and
random reads are the common case.

**Consequence:** lookup no longer depends on the number of fragments. The
interface is unchanged; correctness for gaps is preserved by an explicit
`containsLogicalOffset` check on the candidate.

## D23 — LRU-bounded descriptor cache

**Decision:** `LogicalFile` keeps at most `maxOpenFiles` (default 64) physical
files open, evicting the least recently used. Cache entries hold
`shared_ptr<PosixFile>`, and `read` holds a `shared_ptr` for the duration of
each `pread`.

**Why:** lazy opening without a bound would eventually exhaust
`RLIMIT_NOFILE` for a logical file with thousands of fragments. Eviction keeps
the descriptor count bounded. The `shared_ptr` is what makes eviction safe
under concurrency: evicting a file only removes it from the cache; an in-flight
`pread` keeps it alive until it finishes, so no pointer dangles.

**Consequence:** a workload that cycles through more fragments than the cache
size re-opens files, trading syscalls for a bounded descriptor count. The limit
is configurable through `LogicalFile::open`.

## D24 — Metadata checksum with backward-compatible versioning

**Decision:** format version 2 adds a CRC-32 of the fragment region to the
header. The writer emits version 2; the decoder reads both version 1 (no
checksum) and version 2, and rejects any other version.

**Why:** a checksum turns silent corruption (a flipped bit, a bad sector) into a
detected error (`checksum_mismatch`) instead of a wrong mapping. Making the
change a new *version* rather than an incompatible edit demonstrates the point
of versioning: old files keep working, and the format can evolve without a flag
day.

**Consequence:** the checksum is verified after the bounds-checked decode, so
truncation is reported as truncation and corruption as `checksum_mismatch`. CRC-32
detects accidental damage, not deliberate tampering; it is an integrity check,
not a security mechanism. The checksum covers the fragment region, while header
fields are constrained by explicit validation.

## D25 — Writers are serialised across the whole read-modify-write

**Decision:** metadata mutations take an advisory exclusive `flock` on a sibling
`<metadata>.lock` file. The lock is held for the entire load → mutate → store
cycle (via `updateMetadata`), not only for the final write.

**Why:** atomic replacement protects readers from torn files, but it does not
protect writers from each other. Two processes that both load, append, and
store would each overwrite the other's update — a lost update. The load is part
of the race, so the lock must cover it. `flock` is chosen over `fcntl` because
`flock` locks belong to the open file description, so independent opens (even
within one process) exclude each other, which makes it correct for threads too.

**Consequence:** writers are fully serialised; readers are unaffected (they do
not take the lock and rely on rename atomicity). The lock file is separate from
the metadata because the metadata is replaced by `rename`, which would
invalidate a lock held on the old inode. A crashed writer releases its lock
automatically when the process exits. Cross-host locking still depends on the
filesystem's `flock` support (NFS is the usual caveat).
