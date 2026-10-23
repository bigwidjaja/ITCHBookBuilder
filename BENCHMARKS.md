# Benchmarks

Full-day replay of `12302019.NASDAQ_ITCH50`: 268,744,780 messages, 8.25 GB decompressed.

```
/usr/bin/time -l ./build-rel/itchbook book data/12302019.NASDAQ_ITCH50
```

- Build: Release (`-O3 -DNDEBUG`)
- Machine: Apple M2 (8 cores), 16 GB RAM, macOS 26.6, Apple clang 21.0.0
- Single run, file already in the page cache (it had just been written). Phase 3 adds repeated runs and cold vs warm.
- msg/s is the program's own count over its replay time. Wall time and peak RSS come from `/usr/bin/time -l`.

Every row must replay with 0 errors and reproduce `tests/golden/golden-v1.csv` exactly.

| Version | Change | Wall time | msg/s | ns/msg | Peak RSS | Errors |
| --- | --- | --- | --- | --- | --- | --- |
| v1 baseline | `std::unordered_map` orders, `std::map` price levels, virtual `Handler`, `ifstream` reads | 82.6 s | 3.25 M | 307 | 176 MB | 0 |
