# ITCH Book Builder: Project Plan and Assignment

Oct 12, 2026 · @Ryan

## Part 1: Project plan

### Overview

You will build **itchbook**: a C++ library that rebuilds NASDAQ order books from a full day of real ITCH 5.0 data, makes the rebuild fast, and exposes it to Python and a REST API.

It mirrors the first stages of market data work on a Libraries and Tools team: parse an exchange's binary protocol, manage order books, tune data structures for latency, and build tools other people use.

**What you end up with**

- `itchbook_core`: a C++ library with a parser and two book versions, a simple v1 and a fast v2.
- `itchbook`: a CLI with `count`, `dump`, `stats`, and `book` commands.
- Tests, benchmarks, and a results table showing every optimization and what it bought.
- A Python module (`import itchbook`) built with pybind11, and a FastAPI service on top of it.
- A README with before and after flame graphs and the reasoning behind each design choice.

**Scope rules**

- Correct first, fast second. v1 must produce a saved "golden" output before any optimization starts.
- One change at a time, measured every time.
- Single-threaded. Networking, shared memory, and kernel bypass are stretch goals at the end.

**Architecture**

&#91;embedded content: itchbook architecture · 8 parts\]

The book builder is the part Phases 3 and 4 make fast. Everything to its right consumes it, so every speedup reaches the CLI, Python, and the API.

### Phases and milestones

The project runs in six phases and 14 steps. Each phase ends with a gate you must pass before moving on, and two gates are git tags (`v1.0`, `v2.0`).

Python comes before optimization on purpose: once the bindings exist, every C++ speedup in Phase 4 shows up in Python and the API too.

&#91;embedded content: roadmap · 6 phases, 14 steps, 6 gates\]

Phase 4 is the performance story. Every gate before it exists so that you optimize something you already trust.

| Phase | Doc | Steps |
| --- | --- | --- |
| 0 Setup | [Phase 0](phases/phase-0-setup.md) | 0 |
| 1 Correct (v1) | [Phase 1](phases/phase-1-correct.md) | 1–5 |
| 2 Python and API | [Phase 2](phases/phase-2-python-and-api.md) | 6–7 |
| 3 Measure | [Phase 3](phases/phase-3-measure.md) | 8 |
| 4 Fast (v2) | [Phase 4](phases/phase-4-fast.md) | 9–12 |
| 5 Ship | [Phase 5](phases/phase-5-ship.md) | 13 |

### Environment, tools, and data

Everything runs on Linux. On a Mac, use an Ubuntu 24.04 VM in UTM with 4 or more cores, 8 GB RAM (16 GB if you can), and 60 GB of disk.

Apple Silicon note: inside a VM, perf's hardware counters (like cache misses) are often unavailable. `perf record` still works with software sampling, and valgrind's cachegrind simulates cache misses anywhere.

**Data**

- Spec: [Nasdaq TotalView-ITCH 5.0](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf). Keep it open the whole project; every field offset comes from it.
- Sample files: [NASDAQ ITCH sample directory](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/). Start with `12302019.NASDAQ_ITCH50.gz`, about 3.5 GB compressed and the smallest full-day file from 2019 and 2020. It is several times larger decompressed.
- Encoding (from the spec): integers are big-endian, prices are integers with 4 implied decimals, and timestamps are nanoseconds since midnight.
- File framing (not in the spec, it is how the sample files are stored): every message is preceded by a 2-byte big-endian length.
- Dev subset: `zcat 12302019.NASDAQ_ITCH50.gz | head -c 200M > data/small.itch`. The last message may be cut off, so your reader must stop cleanly.

**Tools**

| Tool | Used for | First step |
| --- | --- | --- |
| CMake + Ninja | Builds, Debug vs Release, build options | 0 |
| Git + GitHub Actions | Version control and CI | 0 |
| bash | Data download, build, and benchmark scripts | 0 |
| GoogleTest | Unit tests and differential tests | 2 |
| gdb | Debugging crashes and wrong books | 2 |
| AddressSanitizer + UBSan | Memory bugs and undefined behavior | 2 |
| `/usr/bin/time -v`, htop | Runtime and peak memory | 5 |
| pybind11 + scikit-build-core | The Python module | 6 |
| FastAPI + uvicorn, curl | The REST service | 7 |
| Google Benchmark | Microbenchmarks | 8 |
| hyperfine | Timing whole CLI runs | 8 |
| perf + FlameGraph scripts | Profiling | 8 |
| valgrind cachegrind | Simulated cache misses | 8 |
| taskset | Pinning benchmarks to one core | 8 |
| Compiler Explorer (godbolt.org) | Reading generated assembly | 12 |

**Repo layout**

```
itchbook/
  CMakeLists.txt
  include/itchbook/   # public headers: messages, parser, book_v1, book_v2
  src/                # library sources (itchbook_core)
  tools/              # the itchbook CLI
  tests/              # GoogleTest
  bench/              # Google Benchmark
  python/             # pybind11 bindings + FastAPI app
  scripts/            # get_data.sh, bench.sh, flamegraph.sh
  notes/              # data-shape.md, experiment notes
  data/               # gitignored
  BENCHMARKS.md
  README.md
```

**Done means**

- A full trading day replays with zero parse errors.
- v2 output matches v1's golden file exactly.
- BENCHMARKS.md has one row per change, each with msg/s and peak memory.
- `pip install .` then `import itchbook` works, and API answers match the CLI.
- CI runs the tests under ASan and UBSan on every push.

**Resume entry (fill in your real numbers)**

**ITCH Book Builder** | C++20, Python, pybind11, FastAPI, Linux, CMake

- Built a C++ NASDAQ ITCH 5.0 parser and limit order book that reconstructs books for every listed symbol from a full trading day (N M messages), reading input via mmap
- Profiled with perf and raised throughput from X to Y M msg/s by replacing node-based STL containers with preallocated, cache-friendly arrays
- Exposed the engine to Python via pybind11 and served book queries through a FastAPI REST API, using periodic snapshots to cut query latency from X ms to Y ms
- Developed on Linux with GoogleTest, Google Benchmark, gdb, and AddressSanitizer; CI via GitHub Actions

## Part 2: The assignment

### How it works

The assignment is 14 steps, done in order. Each step lists its goal, what to read first, tasks, a deliverable, and a checkpoint. Do not start a step until the previous checkpoint passes.

- Commit at the end of every step: `step N: <what you did>`.
- From Step 8 on, every change gets a row in BENCHMARKS.md, even when it made things slower.
- Size is relative effort: S (small), M (medium), L (large).
- Keep a `notes/` file per step with surprises and numbers. Those notes become your README and interview stories.

Work one phase doc at a time. The step text lives in the phase files linked above.

### What you'll learn, and where

Main step is where the topic is taught. Later steps reuse it.

**C++ language and tooling**

| Topic | Main step | Used again |
| --- | --- | --- |
| CMake targets, static libraries, Debug vs Release | 0 | 5, 12 |
| Headers and translation units | 0 | 2 |
| Fixed-width integers (`uint8_t` to `uint64_t`) | 1 | 2 |
| Binary file I/O with `std::ifstream` | 1 | 5 |
| Bit shifts and endianness | 1 | 2, 12 |
| `std::array`, `constexpr` tables | 1 | 2 |
| `std::chrono` timing | 1 | 8 |
| `struct`, `enum class` | 2 | 4 |
| `memcpy` vs `reinterpret_cast`, strict aliasing, undefined behavior | 2 | 9 |
| `std::string_view` | 2 | 3 |
| Unit tests with GoogleTest | 2 | 4, 10 |
| `std::vector`, `std::unordered_map`, sorting with lambdas | 3 | 4 |
| Classes, encapsulation, const correctness | 4 | 10, 11 |
| `std::map`, comparators, iterators | 4 | 11 |
| References vs copies | 4 | 6 |
| `assert`, invariants, unsigned underflow | 4 | 5 |
| Differential testing against a golden file | 5 | 9 to 12 |
| Python bindings and object lifetimes across languages | 6 | 7 |
| RAII and destructors | 9 |  |
| Move semantics, rule of five, deleted copies | 9 |  |
| `std::span` | 9 | 12 |
| Class templates | 10 | 12 |
| `std::lower_bound`, cost of vector insert and erase | 11 |  |
| `sizeof`, padding, alignment | 11 |  |
| Function templates, static vs dynamic polymorphism | 12 |  |
| C++20 concepts | 12 |  |

**High-performance systems**

| Topic | Main step | Used again |
| --- | --- | --- |
| Throughput (msg/s) and ns per message | 1 | 5, 8 |
| Measure your data before choosing data structures | 3 | 10 |
| Release builds and optimization levels | 5 | 12 |
| Batching across a language boundary, the GIL | 6 |  |
| Precomputation: trading memory for query latency | 7 |  |
| Benchmark hygiene: warm vs cold, noise, pinning, `DoNotOptimize` | 8 | 9 to 12 |
| Sampling profilers and flame graphs | 8 | 9 to 12 |
| Amdahl's law: fix the biggest hotspot first | 8 |  |
| Zero-copy I/O, mmap, the page cache | 9 |  |
| Heap allocation cost, `reserve`, preallocation | 10 |  |
| Hash tables: chaining vs open addressing, load factor | 10 |  |
| Cache lines and pointer chasing (node-based vs flat containers) | 10 | 11 |
| Data-oriented design, compact structs | 11 |  |
| Search order and branch prediction | 11 |  |
| Inlining and devirtualization | 12 |  |
| Compiler flags, LTO, reading assembly | 12 |  |

**Linux**

| Topic | Main step | Used again |
| --- | --- | --- |
| Terminal, apt, VM setup | 0 |  |
| bash scripts | 0 | 8 |
| CI on GitHub Actions Ubuntu runners | 0 | 13 |
| gdb: breakpoints, watchpoints, backtraces | 2 | 4 |
| ASan and UBSan | 2 | 13 |
| `/usr/bin/time -v` and htop: runtime, peak memory | 5 | 10 |
| curl, running a local service | 7 |  |
| `perf record`, `perf report`, `perf stat` | 8 | 9 to 12 |
| `taskset`, `lscpu` | 8 |  |
| Dropping the page cache for cold runs | 8 | 9 |
| valgrind cachegrind | 8 | 11 |
| File descriptors: `open`, `fstat`, `close` | 9 |  |
| `mmap`, `munmap`, `madvise` | 9 |  |

**Market data**

| Topic | Main step | Used again |
| --- | --- | --- |
| Length-prefixed message framing | 1 |  |
| Add, execute, cancel, delete, replace messages | 2 | 4 |
| Fixed-point prices (4 implied decimals) | 2 | 4 |
| Timestamps in nanoseconds since midnight | 2 | 4 |
| Stock locate codes and the symbol directory | 3 | 11 |
| Order-by-order data (L3) to price levels (L2) | 4 |  |
| Book invariants: crossed books, negative sizes | 4 | 5 |
| Snapshots and replay | 7 |  |

### Stretch goals

- Fuzz the parser with libFuzzer (`-fsanitize=fuzzer,address` with clang).
- Windows build: hide `MappedFile` behind one interface, implement it with `CreateFileMapping`, and add a `windows-latest` CI job. The job description mentions Windows.
- Two threads: a parser thread and a book thread joined by a lock-free single-producer single-consumer queue (`std::atomic`, acquire and release, false sharing).
- Shared memory: publish top-of-book so other processes can read it.
- Network replay: send the file over UDP multicast with sequence numbers and detect gaps, then try an AF\_XDP receive path (kernel bypass).

### Sources

- [Nasdaq TotalView-ITCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf)
- [NASDAQ ITCH sample files](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/)
- [CppTrader](https://github.com/chronoxor/CppTrader)
