# FragFS

> A zero-copy logical file aggregation layer that maps multiple physical files
> into a single contiguous logical file.

## What is FragFS?

Large files are frequently split across many physical files: multi-part
downloads (`part1.dat`, `part2.dat`, ...), database segments, log rotations,
container layers, or chunks produced by a backup tool. To use them as a single
stream you normally have to concatenate them:

```text
part1.dat ──┐
part2.dat ──┼── COPY ──> combined.dat
part3.dat ──┘
```

That copy is expensive: it doubles the disk footprint, costs time proportional
to the total size, and can fail halfway through.

FragFS represents the same logical file **without copying any data**. It stores
only a mapping:

```text
Logical file: combined.ff

Logical range        Physical mapping
0   - 100 MB    ->   part1.dat : 0 - 100 MB
100 - 150 MB    ->   part2.dat : 0 - 50 MB
150 - 350 MB    ->   part3.dat : 0 - 200 MB
```

Every read is translated on the fly:

```text
logical offset -> fragment -> physical file -> physical offset -> pread()
```

## Why?

- **No duplicate storage.** The physical bytes are never copied.
- **Instant "concatenation".** Creating a logical file only writes metadata.
- **Random access.** A read at any logical offset is served by `pread()` on the
  underlying fragments.
- **Composable.** A fragment can map a *portion* of a physical file, so FragFS
  can expose arbitrary byte ranges, not just whole files.

## Architecture

```text
              ┌───────────────┐
              │      CLI      │
              └───────┬───────┘
                      │
              ┌───────▼───────┐
              │   FragFS Core │
              │               │
              │ Metadata      │
              │ Mapper        │
              │ LogicalFile   │
              └───────┬───────┘
                      │
              ┌───────▼───────┐
              │   POSIX I/O   │
              └───────┬───────┘
                      │
                Physical Files
```

The core is deliberately independent of any filesystem adapter. A future FUSE
(or macOS user-space filesystem) layer plugs into the same core:

```text
              ┌───────────────┐
              │ FUSE Adapter  │
              └───────┬───────┘
                      │
              ┌───────▼───────┐
              │   FragFS Core │
              └───────────────┘
```

## Example

```bash
# Map three physical files into one logical file. No data is copied.
fragfs create combined.ff part1.dat part2.dat part3.dat

# Inspect the mapping.
fragfs info combined.ff

# Read a logical byte range (binary output must go to a file or a pipe).
fragfs read combined.ff 120000000 4096 --output result.bin

# Grow the logical file without touching existing data.
fragfs append combined.ff part4.dat
fragfs add combined.ff video.dat --physical-offset 500000000 --length 100000000

# Drop a fragment; later fragments shift down, physical data is untouched.
fragfs remove combined.ff 1

# Check that every fragment still resolves to a valid physical range.
fragfs verify combined.ff

# Measure read performance.
fragfs benchmark combined.ff
```

## Performance

`fragfs` stores a mapping instead of copying, so creating a logical file costs
a metadata write regardless of the data size. A representative run of the
benchmark suite (4 files x 4 MiB, macOS/arm64, page cache warm):

```text
Creation
  concatenation : 0.021 s  (16777216 bytes copied)
  fragfs build  : 0.000200 s
  fragfs write  : 0.001636 s
  metadata size : 180 bytes
  data copied   : 0 bytes

Sequential read throughput
  concatenated  : 13130.0 MiB/s
  fragfs logical: 11891.1 MiB/s
```

Creation is roughly five orders of magnitude faster and copies no data;
sequential reads are close to a plain concatenated file because both are
limited by the same underlying I/O. Run `./build/benchmarks/fragfs_benchmark`
to reproduce on your own hardware.

## Supported platforms

- Linux
- macOS

Both are POSIX platforms; FragFS uses a single POSIX implementation. Any
platform-specific code lives under `src/platform/`.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Testing

Each test file compiles into its own executable and is registered with CTest:

```bash
ctest --test-dir build --output-on-failure          # run everything
ctest --test-dir build -R fragment_test -V          # one suite, verbose
./build/tests/fragment_test                          # run a suite directly
```

For memory-safety and integer-overflow checking, configure a sanitizer build:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DFRAGFS_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

## Continuous integration and delivery

`.github/workflows/build.yml` runs on every push and pull request:

- Release build + full test suite on **Ubuntu** and **macOS**
- A **Debug build under AddressSanitizer + UndefinedBehaviorSanitizer** on Ubuntu

`.github/workflows/release.yml` provides continuous delivery: pushing a tag
such as `v0.1.0` builds the tagged revision on Linux and macOS, runs the tests,
and attaches the resulting `fragfs` binaries to a GitHub Release.

## Current status

The core engine and all non-mounting commands are implemented: `create`,
`info`, `read`, `append`, `add`, `remove`, `verify`, and `benchmark`. Metadata
updates are crash-safe. A filesystem adapter (`mount`/`unmount`) is not yet
implemented.

## Roadmap

```text
[x] Repository + CMake + CLI skeleton
[x] Fragment data model
[x] Metadata representation + validation
[x] Metadata serialization
[x] Logical-to-physical mapper
[x] POSIX file abstraction
[x] Logical reads
[x] create / info / read commands
[x] append / add / remove commands
[x] verify command
[x] Crash-safe metadata updates
[x] Partial physical ranges
[x] Fragment removal
[x] Concurrency
[x] Performance benchmarks
[x] Linux/macOS CI (+ sanitizers) and tagged releases
[ ] FUSE (Linux) / user-space filesystem (macOS) adapter
```

## License

MIT. See [LICENSE](LICENSE).
