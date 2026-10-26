# ITCHBookBuilder

A C++20 parser and order book builder for NASDAQ TotalView-ITCH 5.0. It rebuilds the order book of every listed stock from a full trading day, and serves it to Python and over a REST API.

- Full day (268.7M messages) replays with 0 errors in 82.6 s, 3.25M msg/s (v1, Apple M2). See [BENCHMARKS.md](BENCHMARKS.md).
- v1 output is pinned by a golden file, `tests/golden/golden-v1.csv`. BookV2 reproduces it exactly, and matches v1 after every message of the day.

## Data

```
scripts/get_data.sh                                  # downloads the 2019-12-30 sample day, writes data/small.itch
gzip -dc data/12302019.NASDAQ_ITCH50.gz > data/12302019.NASDAQ_ITCH50   # the full day, 8.25 GB
```

## Build and test

```
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
ctest --test-dir build-rel
```

Options: `-DITCHBOOK_SANITIZE=ON` (ASan + UBSan), `-DITCHBOOK_BUILD_BENCH=ON` (Google Benchmark).

## CLI

```
itchbook count FILE                                  # messages per type
itchbook dump FILE --type A --limit 20               # decoded messages
itchbook stats FILE                                  # data shape: types, top symbols, busiest second
itchbook book FILE --symbol AAPL --at 10:30:00       # one stock's book at a time of day
itchbook book FILE [--golden out.csv]                # full replay: errors, peak orders, crossed books
itchbook diff FILE                                   # BookV1 vs BookV2 after every message
itchbook parse FILE                                  # parse only, no book (for benchmarks)
```

`book` and `diff` take `--book v1|v2|v2-std|v2-direct|v2-bsearch` (default `v2`), and `book` takes `--dispatch virtual|template`. BookV1 is the frozen v1 that produced the golden file; BookV2 is the Phase 4 book (flat hash map of orders, sorted-vector price levels with a level pool).

## Python

```
python3 -m venv .venv && .venv/bin/pip install '.[api,dev]'
```

```python
import itchbook

book = itchbook.replay("data/12302019.NASDAQ_ITCH50", "AAPL", "10:30:00")
bids, asks = book.top(5)                                 # [(price, shares), ...]
frame = itchbook.top_of_book_frame("data/12302019.NASDAQ_ITCH50", "AAPL", 1.0)  # pandas
```

## REST API

```
.venv/bin/uvicorn itchbook.api:app --port 8000
curl 'localhost:8000/book/AAPL?at=10:30:00&depth=5'
curl 'localhost:8000/bbo/AAPL?start=09:30:00&end=09:31:00&interval=1s'
curl 'localhost:8000/stats'
```

## Benchmarks and profiling

```
scripts/bench.sh [--cold] [--command book|parse|count]   # median of 5 full-day runs
scripts/flamegraph.sh                                    # flame graph of a full-day replay
scripts/measure_phase4.sh                                # every Phase 4 variant, each from its own commit
```

`notes/` has the measurement evidence BENCHMARKS.md cites: the day's data shape, the v1 flame graph and the v1 cachegrind report.
