# Phase 5: data-oriented optimizations

Phase 4 made the book fast enough to be interesting. Phase 5 looks at it as data: how big each record is, which cache lines each message touches, and which work could be skipped. Steps continue the numbering from Phase 4 (Steps 9 to 12), so this phase is Steps 13 to 19.

## Rules for every step

- One step, one commit, so `scripts/measure_phase4.sh` (or a `measure_phase5.sh` built the same way) can add a row that builds exactly that commit.
- Every variant must replay the full day with 0 errors, reproduce `tests/golden/golden-v1.csv` byte for byte, and pass `itchbook diff` against BookV1.
- Keep a change only if it beats noise. On the Mac, runs spread about 7%, so smaller wins need the pinned Linux numbers from Step 13.
- Record experiments that lost too, with their numbers. A rejected idea with a measurement is still a result.

## Before starting

Finish Phase 4's owed rows (BENCHMARKS.md, "Phase 4: owed measurements"). Every step below is measured against the final BookV2, and it has not been timed on the full day yet.

## Step 13: a quieter baseline

What to fix in the measurement before changing the code.

| # | Change | Why |
| --- | --- | --- |
| 13.1 | A bare replay command that runs BookV2 with no `CheckedBook` | `book` checks for a locked or crossed book after every message (about 1.6% of instructions in v1). Timing the book alone makes small wins visible. |
| 13.2 | Time on a Linux machine with `taskset` pinning | macOS has no pinning, which is where the 7% spread comes from. |
| 13.3 | `perf stat` per run: instructions, L1d misses, LLC misses and dTLB misses per message | v1's cachegrind numbers are simulated. Real counters show which of the steps below actually move misses. |
| 13.4 | A probe-length histogram for the FlatMap (debug builds only) | Steps 14.2 and 14.3 trade probe length against footprint. This shows the trade directly. |

## Step 14: order table layout

The FlatMap is hit by every A, F, E, C, X, D and U message, so its layout matters most.

| # | Change | Why | Risk |
| --- | --- | --- | --- |
| 14.1 | 16-byte slots: a `uint32_t` key, the 12-byte `OrderV2`, and a sentinel key for "empty" in place of `bool full` | Today a slot is `uint64_t` key + 12-byte order + `bool` = 21 bytes, padded to **24**, so slots straddle cache lines. At 16 bytes, exactly 4 fit in a line. The reserved table (4,194,304 slots) drops from about 101 MB to 67 MB. | The largest ref on 2019-12-30 is 308,631,072, about 7% of $2^{32}$. Other days may differ, so check every ref and fail loudly (or fall back to a 64-bit table) if one does not fit. |
| 14.2 | Try `ref & mask` (no hash) against the Fibonacci hash | Refs are handed out in increasing order and most orders die young (140.3M created, at most 1.92M live). Without a hash, orders added close together sit in nearby slots, so the deletes that follow hit warm lines. The Fibonacci hash scatters them on purpose. | Long runs of full slots. Use the 13.4 histogram. |
| 14.3 | Try a higher maximum load (0.7, 0.75) once slots are 16 bytes | At peak the table is 46% full. A smaller table fits more of itself in cache, at the cost of longer probes. | Same as 14.2. |
| 14.4 | Stretch: a sliding-window direct array for young orders, with the FlatMap only for old ones | Step 10.5's direct array needed 3.7 GB because it covered every ref. A window over recent refs covers most lookups at a fraction of that. | More code, and two places an order can live. Do it last, and only if 14.1 to 14.3 leave the order table as the top hotspot. |

## Step 15: price levels

| # | Change | Why | Risk |
| --- | --- | --- | --- |
| 15.1 | Shrink `PoolLevel` from 16 bytes to 8 (`uint32_t` shares + `uint32_t` price) | It is `uint64_t` shares + `uint32_t` price + 4 bytes of padding. | A level's total could pass $2^{32}$ shares. Add the largest level total to `itchbook stats` first, then decide. |
| 15.2 | Give each book its own level pool, or keep shares inline in the sorted side and drop the pool | The pool is shared by all 8,906 stocks, so one stock's levels are scattered among everyone else's. Every add goes side vector, then `pool_[index]`: two lines. | Inline shares mean a delete must search its side again (cheap when near the best price, which most are). Measure both against Step 11.4. |
| 15.3 | Small-buffer sides: keep the best few levels of each side inside `Book` | Each side is a `std::vector`, so reading a stock's best level always follows a pointer. | Only worth it if most sides are short. Add a levels-per-side histogram to `stats` first. |

## Step 16: hide memory latency

v1 averaged about 2 last-level misses per message, and the microbenchmarks run about 10 times faster than the real replay, which points at memory latency, not instructions. Steps 14 and 15 shrink the misses. This step hides the ones that are left.

| # | Change | Why | Risk |
| --- | --- | --- | --- |
| 16.1 | Prefetch ahead: while applying message $i$, decode the order ref of message $i+K$ and `__builtin_prefetch` its FlatMap home slot | The file says exactly which orders are coming. Nothing in the repo prefetches today. | Prefetching too early gets evicted, too late does not help. Sweep $K$ = 4, 8, 16, 32. |
| 16.2 | A second stage: prefetch the order's level once its slot has arrived (around $i+K/2$) | The level index lives in the order, so it can only be prefetched after the first prefetch lands. | More bookkeeping in the loop. Only try it after 16.1 is a measured win. |
| 16.3 | Huge pages for the order table and the level pool (`madvise(MADV_HUGEPAGE)` on Linux) | A 67 to 101 MB table read at random needs many 4 KB page translations. 2 MB pages cut the TLB misses. | Linux only. Check dTLB misses (13.3) before and after. |

## Step 17: parsing and dispatch

Reading, decoding and dispatch were 12% of v1's replay, so wins here are smaller but cheap to get.

| # | Change | Why |
| --- | --- | --- |
| 17.1 | Move the `decode_*` functions into the header, or turn on LTO (`-DITCHBOOK_LTO=ON`, row 12.4) | They live in `messages.cpp`, so even with template dispatch the compiler cannot inline them, and it decodes fields the book never reads: the stock symbol, the MPID, the match number, the execution price. |
| 17.2 | A book-only fast path that reads just ref, shares, price, side and locate straight from the bytes | Skips building the decoded structs entirely for the book. Keep the full decoders for `dump` and `stats`. |

## Step 18: do less work

These change what gets computed, not how fast. They help the Python and REST paths most.

| # | Change | Why |
| --- | --- | --- |
| 18.1 | Single-symbol replays apply only that symbol's messages | Python's `replay()` and `top_of_book_series()` and the CLI's `book --symbol` build all 8,906 books to answer one. AAPL is 1.52M of 268.7M messages (under 0.6%). Once the R message names the locate, skip book messages for every other locate. `SnapshotIndex` already does this. |
| 18.2 | Per-symbol message streams for `SnapshotIndex` | A query walks every message from its snapshot's offset and drops the ones for other stocks. Recording each indexed symbol's message offsets (or copying its messages into one contiguous buffer) lets a query read only its own bytes. Measure with `python/scripts/api_latency.py`, and report the extra memory. |
| 18.3 | Store snapshots compacted | Each snapshot copies the whole `BookV2`, including an order table kept at most half full. Copying into an exactly sized table cuts snapshot memory. Memory only, not speed. |

## Step 19 (stretch): split stocks across threads

Every ITCH message carries its stock locate, and a replace keeps its locate, so stocks are independent. One reader thread walks the file and hands each book message to the thread that owns its locate through a lock-free single-producer, single-consumer ring (cache-line padded). Each worker owns its stocks' books and its own, smaller order table.

- Why: the book is 88% of the replay, and it splits cleanly by stock.
- Ceiling: the reader. `count` runs at 30.4M msg/s, about 10 times v1's full replay, so that is roughly the most this can give.
- Check: the golden file is written from one thread after all workers finish a tick, so the output must stay byte for byte the same.

## Suggested order

1. Step 13 (measure properly first).
2. 16.1 (likely the biggest single win, since the replay waits on memory).
3. 14.1, then 14.2 and 14.3.
4. 15.1 and 15.2.
5. 18.1 and 18.2 (largest effect on the API).
6. 17.1 and 17.2, then 16.2 and 16.3.
7. 14.4 and Step 19 if there is time.

After each step: one row in BENCHMARKS.md, and a final flame graph and cachegrind comparison at the end of the phase, as in Phase 4.
