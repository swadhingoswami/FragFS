# FragFS

[![build](https://github.com/swadhingoswami/FragFS/actions/workflows/build.yml/badge.svg)](https://github.com/swadhingoswami/FragFS/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS-lightgrey.svg)
![Tests](https://img.shields.io/badge/tests-14%20suites-brightgreen.svg)

> A zero-copy logical file aggregation layer: map many physical files into one
> logical file, transfer the pieces, and reassemble the original — with **no
> data copy during mapping** and **reflink-based zero-copy reassembly** where
> the operating system allows it.

---

## Table of contents

- [What is FragFS?](#what-is-fragfs)
- [Why](#why)
- [Core concept](#core-concept)
- [Architecture](#architecture)
- [End-to-end workflow](#end-to-end-workflow)
- [Commands](#commands)
- [The manifest](#the-manifest)
- [How reassembly works](#how-reassembly-works)
- [Reliability guarantees](#reliability-guarantees)
- [Performance](#performance)
- [Platform support](#platform-support)
- [Building and testing](#building-and-testing)
- [CI/CD](#cicd)
- [Project layout](#project-layout)
- [Status and roadmap](#status-and-roadmap)
- [Tags](#tags)
- [License](#license)

---

## What is FragFS?

Large files get split into pieces — multi-part downloads, transfer chunks,
backup segments. To use them as one file you normally concatenate, which
**copies every byte** and doubles the storage.

FragFS represents the same file with **only a mapping** (a *manifest*). The
bytes stay where they are. The manifest records, for every chunk, which range
of the original it covers. Reading the original means looking up the range and
reading that chunk.

```text
   Physical chunks                         Logical file (manifest)
   ┌──────────────┐
   │ chunk_000    │──┐
   ├──────────────┤  │   logical [0, 100M)  -> chunk_000
   │ chunk_001    │──┼─▶ logical [100M,150M) -> chunk_001
   ├──────────────┤  │   logical [150M,350M) -> chunk_002
   │ chunk_002    │──┘
   └──────────────┘
        (data)                                   (no data)
```

---

## Why

- **No duplicate storage.** The physical bytes are never copied to make the
  logical file.
- **Instant mapping.** Building the manifest writes only a few hundred bytes,
  regardless of the data size.
- **Integrity.** A CRC-32 per chunk detects a missing or corrupted chunk.
- **Fast reassembly.** On a reflink filesystem the original is reassembled by
  cloning extents — **no bytes are read or written**.
- **Portable.** One binary manifest format, Linux and macOS.

---

## Core concept

There are two distinct operations, and it matters which one copies data:

```text
   ┌───────────────────────────┐        ┌───────────────────────────┐
   │        MAPPING            │        │       ASSEMBLY            │
   │  (build the manifest)     │        │  (produce the original)   │
   ├───────────────────────────┤        ├───────────────────────────┤
   │ reads chunk data:   NO    │        │ reflink FS:  no copy      │
   │ writes chunk data:  NO    │        │ otherwise:   one copy     │
   │ writes manifest:   yes    │        │ writes output: yes        │
   └───────────────────────────┘        └───────────────────────────┘
```

The mapping is the zero-copy heart of FragFS. Assembly is where the original
materialises, and even there FragFS avoids copying when the OS can.

---

## Architecture

Strict dependency layering; the core never depends on the CLI or any
filesystem adapter:

```text
                         ┌───────────────────────────────┐
                         │             CLI               │
                         │  split · get · info · verify  │
                         │  read · append · add · remove │
                         └───────────────┬───────────────┘
                                         │
                         ┌───────────────▼───────────────┐
                         │          FragFS Core          │
                         │                               │
                         │  Manifest  (metadata, v4)     │
                         │  Assembler (reflink/copy)     │
                         │  Mapper    (logical→physical) │
                         │  Verifier  (presence + CRC)   │
                         │  Store     (atomic writes)    │
                         └───────────────┬───────────────┘
                                         │
                         ┌───────────────▼───────────────┐
                         │        Platform I/O           │
                         │  PosixFile  pread/pwrite      │
                         │  cloneRange FICLONERANGE      │
                         │  copyRange  copy_file_range   │
                         └───────────────┬───────────────┘
                                         │
                    ┌──────────┬─────────┴─────────┬──────────┐
                    ▼          ▼                   ▼          ▼
                chunk_000   chunk_001   ...    chunk_N-1   chunk_N
```

Source files:

```text
include/fragfs/    fragment.h  metadata.h  mapper.h  logical_file.h
                   metadata_store.h  operations.h  verify.h  assemble.h
                   posix_file.h  platform_io.h  crc32.h  error.h
src/core/          metadata · mapper · logical_file · metadata_store
                   operations · verify · assemble · crc32 · error
src/platform/      posix_file · platform_io
src/cli/           main · commands
```

---

## End-to-end workflow

```text
   SOURCE MACHINE                         DESTINATION MACHINE
   ──────────────                         ───────────────────

   big.dat
      │
      │  fragfs split big.dat --chunk-size 100MB
      ▼
   ┌──────────┬──────────┬─────┐          ┌──────────┬──────────┬─────┐
   │ big_000  │ big_001  │ ... │  ─────▶  │ big_000  │ big_001  │ ... │
   └──────────┴──────────┴─────┘  scp     └──────────┴──────────┴─────┘
   ┌──────────────────────────┐   (ship   ┌──────────────────────────┐
   │ big.dat.meta (manifest)  │  both)    │ big.dat.meta (manifest)  │
   │  original name + size    │  ─────▶   │  original name + size    │
   │  per chunk: range + CRC  │           │  per chunk: range + CRC  │
   └──────────────────────────┘           └────────────┬─────────────┘
                                                       │
                              fragfs get big.dat ◀─────┘
                                       │
                                       ▼
                                  big.dat   (the original)
```

**Step by step**

```text
  1. split     reads big.dat once, writes chunks + manifest (CRCs computed)
  2. transfer  ship the chunks AND the manifest together
  3. get       read the manifest, check every chunk, reassemble big.dat
  4. consume   (optional) delete the chunks once big.dat is durable
```

The manifest is the **single source of truth**: it lists every chunk, so a
missing chunk is detectable without shipping the data.

---

## Commands

| Command | Purpose |
|---|---|
| `fragfs split <file> --chunk-size <size>` | Split into chunks of a given size (KB/MB/GB) |
| `fragfs split <file> --chunks <n>` | Split into roughly `n` chunks |
| `fragfs get <original> [--consume] [--verify]` | Reassemble the original from the manifest |
| `fragfs info <original>` | Show the manifest (chunks, ranges, CRCs) |
| `fragfs verify <original>` | Check every chunk exists and its CRC matches |
| `fragfs read <original> <offset> <size> [--output f]` | Read a logical range |
| `fragfs <logical> <chunk>...` | Manual mapping (no CRCs) — build a manifest by hand |
| `fragfs append <logical> <file>` | Add one physical file to a manifest |
| `fragfs add <logical> <file> --physical-offset N --length M` | Add a partial range |
| `fragfs remove <logical> <id>` | Remove a fragment |
| `fragfs benchmark <logical>` | Measure read performance |

---

## The manifest

A compact, versioned binary sidecar named `<original>.meta`, created by `split`
and shipped with the chunks.

```text
  header
  ┌────────┬─────────┬───────┬──────────────┬───────────────┬────────────┐
  │ magic  │ version │ flags │ original_size│ fragment_count│ checksum   │
  │  8 B   │   4 B   │  4 B  │     8 B      │      8 B      │    4 B     │
  └────────┴─────────┴───────┴──────────────┴───────────────┴────────────┘
  ┌─────────────────────────────┐
  │ original_name (length + bytes)│
  └─────────────────────────────┘
  fragments (repeated)
  ┌───────────────┬────────────────┬────────┬───────────────┬──────────┐
  │ logical_start │ physical_start │ length │ path (len+str)│ crc32    │
  │     8 B       │      8 B       │  8 B   │   variable    │   4 B    │
  └───────────────┴────────────────┴────────┴───────────────┴──────────┘
```

- All integers are **little-endian**; no raw C++ structs are written.
- `version` is checked; unknown versions are rejected.
- The decoder validates every length against the remaining buffer before use.
- v1–v3 files remain readable; v4 is written by `split`.

Example (`fragfs info`):

```text
Original name: swadhin.dat
Logical file : swadhin.dat
Logical size : 55 bytes
Fragments    : 6

ID   File           Logical Start   Physical Start  Length   CRC32
0    swadhin_000    0               0               10       a3ec1434
1    swadhin_001    10              0               10       bab8bd00
2    swadhin_002    20              0               10       e132333a
...
```

---

## How reassembly works

`fragfs get` walks the manifest and, for each chunk, uses the **fastest method
the OS offers**:

```text
                     for each fragment
                            │
              ┌─────────────▼──────────────┐
              │  reflink supported?        │
              │  (Linux + btrfs/XFS)       │
              └───────┬───────────┬────────┘
                    yes           no
                      │            │
          ┌───────────▼──┐   ┌─────▼───────────────┐
          │ FICLONERANGE │   │ copy_file_range      │
          │ 0 bytes      │   │ (kernel copy; else   │
          │ read/written │   │  pread/pwrite loop)  │
          └──────────────┘   └──────────────────────┘
                      │            │
                      └─────┬──────┘
                            ▼
                 write to big.dat.tmp
                 fsync → rename → fsync dir   (atomic)
                            │
                    --consume? ──▶ unlink chunks
```

Order of preference:

```text
   reflink (0 copy)  >  copy_file_range (1 kernel copy)  >  pread/pwrite (1 copy)
```

Integrity is checked by default where data is copied; `--verify` forces a
CRC pass even on the reflink path.

---

## Reliability guarantees

```text
   ┌────────────────────┬──────────────────────────────────────────────┐
   │ risk               │ mitigation                                   │
   ├────────────────────┼──────────────────────────────────────────────┤
   │ missing chunk      │ manifest lists all chunks; get/verify report │
   │ corrupt chunk      │ CRC-32 per chunk, checked during assembly    │
   │ half-written file  │ assemble to .tmp, then atomic rename         │
   │ crash mid-update   │ manifest written tmp → fsync → rename        │
   │ lost chunks        │ --consume deletes only after output durable  │
   │ bad metadata       │ bounds-checked decode; versioned format      │
   │ concurrent writers │ advisory flock over the whole update         │
   └────────────────────┴──────────────────────────────────────────────┘
```

---

## Performance

`benchmarks/fragfs_benchmark` builds N chunks, then compares the traditional
"concatenate everything" approach against FragFS mapping + reassembly.

```bash
./build/benchmarks/fragfs_benchmark --files 8 --size-mib 16
```

Representative run (128 MiB total, macOS/arm64, page cache warm):

```text
-------------------------------------------------------------
operation                    normal (copy)    fragfs
-------------------------------------------------------------
map / concatenate            0.1650 s         0.0003 s
data copied (map step)       134217728 B      0 B
extra storage                134217728 B      604 B
reassemble                   0.1650 s         0.0420 s
reassembly method            read+write       copy
-------------------------------------------------------------

Sequential read throughput
  combined file : 1943.1 MiB/s
  fragfs logical: 15142.1 MiB/s

Random 4096-byte reads : 1.14 us/read over 20000 reads
```

Reading the table:

- **The mapping step copies nothing** and takes microseconds, versus a full copy
  of the data. Extra storage is a 604-byte manifest, not a second 128 MiB file.
- **Reassembly** is where data moves. On this macOS/APFS machine there is no
  range-clone primitive, so it copies (still a single streaming pass). On
  Linux + btrfs/XFS the same step reports `reflink (0 copy)`.
- Sequential reads through FragFS are fast because the mapper issues one
  `pread` per fragment; the numbers here reflect the page cache.

Cost model:

```text
   mapping cost    = O(number of chunks)      metadata only
   reflink cost    = O(number of chunks)      metadata only  (btrfs/XFS)
   copy cost       = O(total bytes)           one pass       (everywhere else)
```

So fewer, larger chunks make the zero-copy path cheaper, and reflink never
touches the data at all.

## Platform support

```text
   platform / filesystem     reassembly method        data copied   chunks deletable
   ───────────────────────────────────────────────────────────────────────────────
   Linux  + btrfs / XFS      FICLONERANGE (reflink)   none          yes
   Linux  + ext4 / tmpfs     copy_file_range          one copy      yes
   macOS  + APFS             copy_file_range*         one copy      yes
   NFS / SMB                 server-side copy         one copy      yes
```

`*` macOS has no public range-clone API; only whole-file clones, which cannot
concatenate. So on macOS the copy is unavoidable — a physical limit of the OS,
not of FragFS.

> **The hard rule:** on a non-reflink filesystem you cannot have all three of
> *a regular file*, *chunks deleted*, and *zero copy*. FragFS picks "regular
> file + chunks deleted" and pays one copy where reflink is unavailable.

---

## Building and testing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitizer build:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DFRAGFS_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

Test suites (14): fragment, metadata, serialization, mapper, posix_file,
logical_file, create, operations, verify, concurrency, writer_lock, split,
assemble, smoke.

---

## CI/CD

```text
   push / PR  ──▶  GitHub Actions
                    ├── ubuntu-latest : release build + ctest
                    ├── macos-latest  : release build + ctest
                    └── ubuntu-latest : Debug + ASan/UBSan + ctest

   tag v*     ──▶  build on Linux + macOS, attach binaries to a Release
```

---

## Project layout

```text
fragfs/
├── CMakeLists.txt
├── include/fragfs/      public headers
├── src/core/            mapping, manifest, assembly, verification
├── src/platform/        POSIX I/O, reflink, kernel copy
├── src/cli/             command-line front end
├── tests/               unit + integration tests
├── benchmarks/          performance comparison
├── docs/                architecture, file format, decisions
└── .github/workflows/   CI and release
```

---

## Status and roadmap

```text
[x] Fragment model, metadata, validation
[x] Versioned binary manifest (v4) with per-chunk CRC-32
[x] Logical-to-physical mapper (binary search)
[x] POSIX file abstraction, logical reads
[x] split / get / info / verify / read / append / add / remove / benchmark
[x] Crash-safe writes, writer serialisation, concurrent reads
[x] Reflink fast path + copy_file_range fallback (tiered assembler)
[x] Linux/macOS CI (+ sanitizers) and tagged releases
[ ] Filesystem adapter (mount) — design in docs/fuse-adapter.md
```

---

## Tags

```text
#fragfs #zerocopy #zero-copy #logicalfile #logicalfilesystem #fileaggregation
#filemapping #filesystem #storage #dataintegrity #integrity #checksum #crc32
#reflink #copyonwrite #cow #posix #pread #pwrite #randomaccess #sparsefile
#cpp #cpp17 #moderncpp #cmake #cli #commandline #linux #macos #crossplatform
#systemsprogramming #systems #lowlevel #performance #benchmark #opensource #mit
```

## License

MIT. See [LICENSE](LICENSE).
