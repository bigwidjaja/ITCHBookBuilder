# Phase 3: Measure

[Back to the plan](../ITCH%20Book%20Builder%20Project%20Plan%20and%20Assignment.md)

#### Step 8: Benchmarks and profiling baseline (M)

**Goal.** Know exactly where v1 spends its time before changing anything.

**Learn.** Benchmark hygiene, sampling profilers, flame graphs, Amdahl's law, cache simulation.

**Tasks**

1. Write `scripts/bench.sh`: Release build, full-day replay 5 times pinned with `taskset -c 2`, report the median msg/s. `hyperfine` can do the repeats for you.
2. Separate warm and cold runs. For a cold run, drop the page cache first: `sync; echo 3 | sudo tee /proc/sys/vm/drop_caches`.
3. Split the cost: time `count` (parse only) against the full replay (parse plus book). The difference is the book's share.
4. Google Benchmark microbenchmarks for `BookV1` add, execute, and delete on synthetic orders clustered near the best price. Use `benchmark::DoNotOptimize` so the compiler cannot delete the work.
5. Profile: build with `-O2 -g -fno-omit-frame-pointer`, run `perf record -g` on the full replay, read `perf report`, and make a flame graph with Brendan Gregg's FlameGraph scripts (`scripts/flamegraph.sh`).
6. Run `perf stat` for cycles, instructions, and cache misses if your machine supports them. If not, run `valgrind --tool=cachegrind` on `small.itch`.
7. Write BENCHMARKS.md: the baseline row, the parse vs book split, and the top 3 hotspots with their share of time.

**Deliverable.** BENCHMARKS.md with the baseline and the first flame graph.

**Checkpoint.** You can say in one sentence where most of the time goes and what share it is.

**What to expect.** Hash lookups, tree inserts and erases, and `malloc`/`free` usually dominate. Your profile decides; fix the biggest one first.

Next: [Phase 4: Fast (v2)](phase-4-fast.md)
