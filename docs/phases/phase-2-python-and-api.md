# Phase 2: Python and API

[Back to the plan](../ITCH%20Book%20Builder%20Project%20Plan%20and%20Assignment.md)

#### Step 6: Python bindings (M)

**Goal.** `import itchbook` from Python and get book data into pandas.

**Learn.** pybind11 basics, building a Python package with CMake, object lifetimes across languages, batching, the GIL.

**Read first.** pybind11's docs on functions, classes, and NumPy arrays (`py::array_t`).

**Tasks**

1. Add pybind11 through `FetchContent` and a module target in `python/`. Add a `pyproject.toml` using scikit-build-core so `pip install .` builds it.
2. Expose `replay(path, symbol, until)` returning a `Book` object with `.top(depth)`, which returns lists of `(price, shares)`.
3. Expose `top_of_book_series(path, symbol, interval)` returning a dict of NumPy arrays: time, bid price, bid size, ask price, ask size. Turn it into a DataFrame in Python.
4. Release the GIL around the replay loop with `py::gil_scoped_release`.
5. Experiment: add `replay_with_callback(path, fn)` that calls a Python function once per message. Time it against the batched version on `small.itch` and write the ratio in `notes/`.

**Deliverable.** A short Python script that prints the AAPL book at 10:30 and the first rows of its top-of-book DataFrame.

**Checkpoint.** Python results match the CLI exactly. The batched call runs close to CLI speed; the per-message callback is far slower.

**Hint.** Return copies of small results (lists, arrays) rather than references into C++ objects. It avoids lifetime bugs while you learn.

#### Step 7: FastAPI service with snapshots (M)

**Goal.** A REST API that answers book queries fast by not replaying from the start of the day.

**Learn.** Precomputation, the memory vs latency tradeoff, REST design, curl.

**Tasks**

1. In C++, write `SnapshotIndex`. It replays once and, every N minutes of exchange time, stores a copy of each chosen symbol's book, that symbol's live orders, and the file offset.
2. Query: find the latest snapshot before time t, copy it, then replay from its offset to t, applying only that symbol's messages.
3. Bind it to Python.
4. FastAPI app: `GET /book/{symbol}?at=HH:MM:SS&depth=5`, `GET /bbo/{symbol}?start=...&end=...&interval=1s`, `GET /stats`. Build the index at startup.
5. Measure 100 random `/book` queries with no snapshots and with N = 30, 5, and 1 minutes. Record p50 and p99 latency and snapshot memory for each.

**Deliverable.** A small table in `notes/` showing memory vs latency for each N.

**Checkpoint.** `curl` results match the CLI, and the table shows the tradeoff clearly.

**Why live orders too.** A snapshot of price levels alone is not enough. Later executes and deletes refer to order reference numbers, so the snapshot must hold that symbol's orders as well.

Next: [Phase 3: Measure](phase-3-measure.md)
