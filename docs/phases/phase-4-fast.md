# Phase 4: Fast (v2)

[Back to the plan](../ITCH%20Book%20Builder%20Project%20Plan%20and%20Assignment.md)

Every step in this phase follows the same loop: change one thing, check the golden file, benchmark, write a row. A typical BENCHMARKS.md looks like this:

```markdown
| Version | Change | Median msg/s | ns/msg | Peak RSS (MB) | Notes |
| --- | --- | --- | --- | --- | --- |
| v1 | baseline | | | | ifstream, unordered_map, std::map |
| v1.1 | mmap reader | | | | warm cache |
| v1.2 | orders.reserve() | | | | |
```

#### Step 9: mmap and zero-copy parsing (M)

**Goal.** Stop copying every message. Let the parser walk the file in memory.

**Learn.** RAII, destructors, move semantics, rule of five, `std::span`, file descriptors, `mmap`, `madvise`, the page cache.

**Tasks**

1. Write `class MappedFile`. The constructor calls `open`, `fstat` for the size, `mmap` with `PROT_READ` and `MAP_PRIVATE`, and `madvise(MADV_SEQUENTIAL)`. The destructor calls `munmap` and `close`.
2. Delete the copy constructor and copy assignment, and implement the move constructor and move assignment.
3. Expose `std::span<const uint8_t> bytes()`, and make the parser walk a pointer through it. Decode functions already take `const uint8_t*`, so no per-message copy remains.
4. Snapshot offsets from Step 7 become offsets into the mapping.
5. Benchmark warm and cold against the `ifstream` version.

**Deliverable.** A new BENCHMARKS.md row, with warm and cold numbers.

**Checkpoint.** Golden file identical. No leaks under ASan. Moving a `MappedFile` compiles; copying one does not.

#### Step 10: Order store: allocation and hashing (L)

**Goal.** Make order lookups cheap.

**Learn.** Node-based vs flat containers, heap allocation cost, open addressing, load factor, class templates, differential testing.

**Tasks.** One change per benchmark row:

1. `orders.reserve(peak_live_orders)` using the number from Step 4. Measure.
2. Write `FlatMap<K, V>`: one flat array, linear probing, power-of-two capacity, and a simple multiplicative hash (multiply by `0x9E3779B97F4A7C15`, keep the high bits). Handle deletes with backward-shift deletion or tombstones, and write down why you chose one.
3. Differential test: 10 million random inserts, finds, and erases against `std::unordered_map`. Every result must match.
4. Swap `FlatMap` into the book and measure.
5. Experiment: a `std::vector<Order>` indexed directly by order reference, if Step 3's largest reference number fits in memory. Measure speed and peak RSS, then decide and write down why.

**Deliverable.** Rows for each variant, with msg/s and peak memory.

**Checkpoint.** Golden file identical for every variant you keep.

#### Step 11: Price levels, book layout, struct size (L)

**Goal.** Make level updates cheap and the data compact.

**Learn.** Data-oriented design, `sizeof`, padding, alignment, `std::lower_bound`, the cost of vector insert and erase, branch prediction.

**Tasks.** One change per benchmark row:

1. Store books in a `std::vector<BookV2>` indexed by stock locate instead of a map.
2. Price levels: a `std::vector<Level>` sorted so the best price sits at the back (bids ascending, asks descending). Most updates land near the best price, so inserts and erases near the back move few elements.
3. Find a level by scanning backward from the best price, then try `std::lower_bound`. Measure both and keep the faster one.
4. Print `sizeof(Order)`, reorder fields to remove padding, and use the smallest types that fit. Measure.
5. Compare cachegrind cache-miss counts for v1 and v2 on `small.itch`.

**Deliverable.** Rows for each change, plus the cachegrind comparison.

**Checkpoint.** Golden file identical, and you can explain with numbers why the faster version wins.

**Reference.** After your own attempt, compare with the optimized market manager notes in [CppTrader's README](https://github.com/chronoxor/CppTrader). Read for ideas, but keep your code your own.

#### Step 12: Templates, inlining, compiler flags (M)

**Goal.** Remove the last layers of overhead and see what the compiler actually does.

**Learn.** Function templates, static vs dynamic polymorphism, C++20 concepts, inlining, devirtualization, `-O3`, `-march=native`, LTO, reading assembly.

**Tasks**

1. Make the parser a template, `template <class Handler> void parse(std::span<const uint8_t>, Handler&)`, constrained by a C++20 concept that requires `on_add`, `on_execute`, and the rest. Benchmark it against the virtual `Handler` from Step 2.
2. Make the book type a template parameter of the replay loop so v1 and v2 share one code path.
3. Paste `load_be32` into Compiler Explorer at `-O2` and confirm it compiles to a single byte-swap instruction (`bswap` on x86, `rev` on ARM).
4. Try `-O3`, `-march=native`, and LTO (`CMAKE_INTERPROCEDURAL_OPTIMIZATION`), one at a time.
5. Make a final flame graph and put it next to the Step 8 one.
6. Rerun the Step 7 API latency test. Your C++ speedups should show up there too.
7. Tag `v2.0`.

**Deliverable.** BENCHMARKS.md tells the full story from v1 to v2.

**Checkpoint.** Golden file identical, and every row has a one-line explanation.

Next: [Phase 5: Ship](phase-5-ship.md)
