# Benchmarks

Full-day replay of `12302019.NASDAQ_ITCH50`: 268,744,780 messages, 8.25 GB decompressed.

- Build: Release (`-O3 -DNDEBUG`), Apple clang 21.0.0
- Machine: Apple M2 (8 cores), 16 GB RAM, macOS 26.6
- msg/s is messages divided by the median of 5 full-day runs (`scripts/bench.sh`), each after one unmeasured warm-up run. Peak RSS is from one `/usr/bin/time -l` run.
- Runs are **unpinned**: macOS has no `taskset`. Run-to-run spread on this machine was about 7% (82.6 s to 88.9 s for the same binary), so differences smaller than that are noise.
- Every row must replay with 0 errors and reproduce `tests/golden/golden-v1.csv` byte for byte.

## Results

| Version | Change | Wall time | msg/s | ns/msg | Peak RSS | Errors |
| --- | --- | --- | --- | --- | --- | --- |
| v1 baseline | `std::unordered_map` orders, `std::map` price levels, virtual `Handler`, `ifstream` reads | 88.85 s | 3.02 M | 331 | 176 MB | 0 |

The `book` command used for this row runs a `CheckedBook`, which also checks for a locked or crossed book after every message. That check is about 1.6% of the instructions (`BookV1::state` in the cachegrind report), so the bare book is slightly faster than this row.

## Phase 4: owed measurements

Steps 9 to 12 are built and checked for correctness: every variant reproduces the golden file, and the final v2 matches v1 after every message of the full day (`itchbook diff`). They have **not been timed** on the full day yet. This Mac was under memory pressure (swap in use, about 19 GB compressed), the 8.25 GB day no longer stayed in the page cache, and full-day times were mostly disk reads.

With memory freed (close heavy apps; the script warns if the file will not fit):

```
scripts/measure_phase4.sh            # every row below, each built from its own commit
scripts/bench.sh --cold --command count    # Step 9 cold runs (and --command parse/book)
scripts/flamegraph.sh data/12302019.NASDAQ_ITCH50 notes/flamegraph-v2.svg   # Step 12 task 5
scripts/cachegrind.sh data/small.itch book  # Step 11 task 6, on v2 (v1 is in notes/cachegrind-v1.txt)
python python/scripts/api_latency.py        # Step 12 task 6, now on BookV2
```

| Step | Rows owed |
| --- | --- |
| 9 | `ifstream` vs mmap for `count`, `parse`, `book`, warm and cold. Preliminary, on the first 2 GB of the day, both builds alternating: `count` 3.8x to 5.4x faster, `parse` 2.5x to 2.7x, `book` 8% to 15% |
| 10 | BookV2 setup, `reserve`, FlatMap, direct array (decide FlatMap or direct from speed and peak memory) |
| 11 | books by locate, sorted vectors with scan, with binary search (keep the faster), level pool, 12-byte orders; cachegrind v1 vs v2 |
| 12 | virtual vs template dispatch, `-O2` / `-O3` / `-march=native` / LTO, final flame graph, API latency rerun; then tag `v2.0` |

## Where the time goes

Same file, same binary, three commands (`scripts/bench.sh --command ...`, median of 5 warm runs):

| Command | What it does | Time | msg/s | ns/msg | Share of the replay |
| --- | --- | --- | --- | --- | --- |
| `count` | read and frame each message, count its type | 8.85 s | 30.4 M | 33 | 10% |
| `parse` | `count` plus decode and virtual `Handler` calls | 10.76 s | 25.0 M | 40 | 2% more |
| `book` | `parse` plus `BookV1` (and the cross check) | 88.85 s | 3.02 M | 331 | **88% more** |

Reading, decoding and dispatch are **12%** of the replay. The book is the other **88%**.

## Warm vs cold

| Command | Warm (5 runs) | Cold (3 runs) |
| --- | --- | --- |
| `count` | 8.85 s | 8.67 s |
| `book` | 88.85 s | 87.35 s |

No difference. This is a weak test of cold reads: `sudo purge` needs a password here, so the cold runs read a fresh `ditto --nocache` copy of the file, which the page cache had not held. That copy should come from the SSD, but I did not verify it. The SSD is also fast enough that it would not be the limit: even `count` needs only 8.25 GB in 8.7 s, about 950 MB/s.

## Hotspots

Flame graph: `notes/flamegraph-v1.svg`, from `scripts/flamegraph.sh` (`/usr/bin/sample` at 1 ms for 60 s, starting 5 s into the replay, on a `-O2 -g -fno-omit-frame-pointer` build). The leaf-frame share of samples:

| # | Function | Share | What is in it (inlined) |
| --- | --- | --- | --- |
| 1 | `BookV1::remove_from_level` | **39.2%** | `std::map` find and erase on a price level |
| 2 | `std::__hash_table` | **19.2%** | `orders_` lookup, insert and erase; `books_` lookup |
| 3 | `BookV1::add_order` | **11.2%** | `std::map` insert, hash emplace |
| | reading: `read`, `xsgetn`, `MessageReader::next`, `sentry`, `memmove` | about 10% | |
| | `malloc` and `free` | about 5% | |

The top three are 70% of the samples, and all three are `std::map` and `std::unordered_map` operations. `remove_from_level` is the biggest even though deletes are not the most common message, because it also runs for every execute, cancel and replace.

## Cache misses

`scripts/cachegrind.sh` (valgrind cachegrind in an Ubuntu container, simulated L1d 64 KiB, last level 16 MiB; full report in `notes/cachegrind-v1.txt`). Cachegrind simulates a cache, so these are for comparing functions and versions, not exact.

| | Full day | `small.itch` (pre-market) |
| --- | --- | --- |
| Instructions per message | 1,081 | 970 |
| L1 data misses per message | 10.0 | 4.7 |
| **Last-level data misses per message** | **2.08** | **0.51** |

Share of last-level data read misses by code (full day):

| Code (by the header the missing instruction came from) | Share |
| --- | --- |
| Hash table: `hashtable.h` 38% + `hashtable_policy.h` 17% | 55% |
| Comparators: `stl_function.h` (`equal_to` for the hash table, `less` and `greater` for the trees; not separable here) | 15% |
| Red-black tree: `stl_tree.h` | 19% |
| `malloc` and `free` | 5% |

By function (inlined code included), `add_order` accounts for 53% of the last-level read misses, and the hash table code inside it alone is 26%. `remove_from_level` is 13%. About a third of all instructions (35%) have no symbol in the report, mostly libstdc++ and libc code such as the tree rebalancing, so the instruction counts say less than the miss counts.

## Microbenchmarks

`build-rel/itchbook_bench` (Google Benchmark, `-DITCHBOOK_BUILD_BENCH=ON`). One stock's book, synthetic orders within a few cents of $100.00, `benchmark::DoNotOptimize` on the result. The argument is how many orders already rest in the book.

| Benchmark | 1K resting | 16K resting | 1M resting |
| --- | --- | --- | --- |
| `BM_Add` (per add) | 29 ns | 33 ns | 30 ns |
| `BM_Delete` (per delete) | 32 ns | 31 ns | 30 ns |
| `BM_Execute` (per execute, random resting order) | 12 ns | 16 ns | 19 ns |
| `BM_AddThenDeleteOldest` (per add + delete pair) | 62 ns | 51 ns | 54 ns |

These are about **10 times faster per operation** than the real replay, which averages roughly 290 ns per message in the book. The microbenchmark book is one stock with sequential order numbers and no one else's data competing for the cache, so it almost never misses. The real day has 8,906 stocks, up to 1.9M live orders spread over memory, and 2.08 last-level misses per message. That difference is consistent with the real replay being limited by memory latency, not by instructions. The microbenchmarks do not test that directly.

## Summary: where v1 spends its time

**88% of the replay is the book, mostly `std::map` and `std::unordered_map` operations (three functions are 70% of samples), and the cache numbers say those operations spend their time waiting on memory: about 2 last-level data misses per message, against 0.5 on a small book.** Reading and decoding are only 12%.

What this suggests trying first in Phase 4, from the profile (not yet tried):

1. `books_` is an `unordered_map<uint16_t, Book>`, but locates are exactly 1 to 8,906 with no gaps, so a plain vector indexed by locate does the same job with no hashing.
2. The order table's nodes are individually allocated and scattered, so each lookup is a cache miss. A flat open-addressing table keeps them together.
3. Price levels in a `std::map` are one allocation each. Sorted vectors keep the few levels near the best price in a couple of cache lines.
