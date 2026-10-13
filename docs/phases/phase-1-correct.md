# Phase 1: Correct (v1)

[Back to the plan](../ITCH%20Book%20Builder%20Project%20Plan%20and%20Assignment.md)

#### Step 1: Read the file, count the messages (S)

**Goal.** Walk every message in the file and count them by type.

**Learn.** Binary file I/O, fixed-width integers, endianness, `std::array`, `constexpr`, `std::chrono`.

**Read first.** The spec's Data Types section, and how big-endian differs from your CPU's little-endian.

**Tasks**

1. Open the file with `std::ifstream f(path, std::ios::binary)`.
2. Loop: read 2 bytes, build the length as `(b[0] << 8) | b[1]`, then read that many bytes into a buffer.
3. Count `counts[buf[0]]++` in a `std::array<uint64_t, 256>`.
4. Add a `constexpr` table of expected lengths from the spec and count mismatches: S 12, R 39, H 25, Y 20, L 26, V 35, W 12, K 28, J 35, h 21, A 36, F 40, E 31, C 36, X 23, D 19, U 35, P 44, Q 40, B 19, I 50, N 20, O 48. (Lowercase `h` is a different message from `H`.)
5. Stop cleanly if the file ends mid-message, and report it.
6. Print total messages, counts per type, elapsed time, and msg/s.

**Deliverable.** `itchbook count FILE`.

**Checkpoint.** Zero length mismatches on the full file. A, D, E, U, and X are the big counts. Write the full-day msg/s in `notes/` as your first number.

**Hint.** This version is slow on purpose. The gap between it and your final version is the story.

#### Step 2: Decode messages into structs (M)

**Goal.** Turn raw bytes into typed structs and print them, like a packet decoder.

**Learn.** `struct`, `enum class`, `memcpy` vs `reinterpret_cast`, strict aliasing, undefined behavior, `std::string_view`, GoogleTest, gdb, sanitizers.

**Read first.** Why casting a byte pointer to a struct pointer is undefined behavior: alignment, strict aliasing, padding, and byte order. cppreference's `reinterpret_cast` page covers type aliasing.

**Tasks**

1. Write `load_be16`, `load_be32`, `load_be48`, and `load_be64`. Each takes `const uint8_t*` and builds the value with shifts. Test each with hand-written bytes.
2. Define structs for the book-relevant messages: add order (A and F), executed (E), executed with price (C), cancel (X), delete (D), replace (U), stock directory (R), and system event (S). Use `enum class Side : char { Buy = 'B', Sell = 'S' };`.
3. Write one decode function per type using the spec's offsets. Every message shares the same header: type at 0, stock locate at 1, tracking number at 3, timestamp at 5 (6 bytes).
4. Write `parse_message(const uint8_t* p, Handler& h)`: a `switch` on `p[0]` that decodes and calls `h.on_add(...)`, `h.on_execute(...)`, and so on. For now, `Handler` is a base class with virtual functions. You revisit this in Step 12.
5. Helpers: format timestamps as `HH:MM:SS.nnnnnnnnn` and prices as dollars with 4 decimals. Trim the space padding off 8-byte symbol fields into a `std::string_view`.
6. Add `itchbook dump FILE --type A --limit 20`.
7. Tests: one hand-built byte array per message type, checking every field.
8. Build with `-DITCHBOOK_SANITIZE=ON` and run `dump` on `small.itch`.

**Deliverable.** Readable `dump` output, and all tests passing under ASan and UBSan.

**Checkpoint.** Timestamps increase, prices look like real stock prices, and symbols print without trailing spaces.

**gdb exercise.** Shift one offset by a byte on purpose. Break in your decode function, inspect the raw bytes with `x/36xb p`, then fix it.

#### Step 3: Symbol directory and stats (S)

**Goal.** Name every symbol and measure the shape of the data before designing any data structure.

**Learn.** Vectors indexed by an id, `std::unordered_map`, sorting with lambdas, measuring data first.

**Read first.** The spec's Architecture section on stock locate codes: they start at 1, stay fixed for the day, and are meant to be used as array indexes.

**Tasks**

1. On each R message, store the symbol by stock locate in a `std::vector<std::string>`.
2. `itchbook stats FILE` prints messages per type, the top 10 symbols by message count, the busiest second, the largest order reference number, and the number of symbols.
3. Save the output to `notes/data-shape.md`.

**Deliverable.** Stats output for the full day.

**Checkpoint.** The top symbols are tickers you recognize as heavily traded.

**Why it matters.** The largest order reference number decides whether a direct-indexed array is possible in Step 10.

#### Step 4: Order book v1 (L)

**Goal.** A simple, correct book for every symbol.

**Learn.** Classes, `std::map` with a comparator, iterators, references vs copies, invariants, unsigned underflow, gdb watchpoints.

**Read first.** The spec's Add Order and Modify Order sections. Modify messages are cumulative, and an order leaves the book when its shares reach zero.

**Design**

```cpp
struct Order { uint16_t locate; Side side; uint32_t price; uint32_t shares; };

struct Book {
  std::map<uint32_t, uint64_t, std::greater<>> bids;  // price -> total shares, best first
  std::map<uint32_t, uint64_t> asks;                   // price -> total shares, best first
};

std::unordered_map<uint64_t, Order> orders;  // order reference -> order
std::unordered_map<uint16_t, Book> books;    // stock locate -> book
```

**Rules**

- A and F: insert the order and add its shares to its price level.
- E and C: subtract executed shares from the order and its level. For C, use the order's own price level, not the execution price.
- X: subtract canceled shares.
- D: subtract all remaining shares and erase the order.
- U: remove the old order completely, then add the new reference number with the old locate and side and the new price and shares.
- Erase a level when it reaches zero, and an order when its shares reach zero.

**Tasks**

1. Write class `BookV1` with `on_add`, `on_execute`, `on_cancel`, `on_delete`, `on_replace`, and `top(locate, depth)`.
2. Add Debug-only `assert`s that you never subtract more shares than an order or level has. Unsigned subtraction wraps to a huge number instead of going negative.
3. Tests: scripted sequences (add, partial execute, replace, delete) with expected levels.
4. Track the peak number of live orders and add it to `stats`.
5. Add `itchbook book FILE --symbol AAPL --at 10:30:00 --depth 5`, which replays to that time and prints both sides.

**Deliverable.** `book` output for a few symbols and times.

**Checkpoint.** For liquid symbols during market hours, best bid stays below best ask. Count every crossed moment and investigate any you find.

**gdb exercise.** When a level looks wrong, set a watchpoint on its quantity and find which message changed it.

#### Step 5: Full-day run and golden file (M)

**Goal.** A trusted v1 baseline that every later change is tested against.

**Learn.** Release vs Debug builds, peak memory, differential testing.

**Tasks**

1. Make a Release build (`-DCMAKE_BUILD_TYPE=Release`) in `build-rel/`. Run it once against Debug, just to see the gap.
2. Run the full day under `/usr/bin/time -v`. Record wall time, msg/s, and maximum resident set size.
3. Add `--golden out.csv`: for 5 liquid symbols, every 60 seconds of exchange time, write the time, symbol, and top 5 levels per side.
4. Fix anything that breaks on the full file.
5. Save the golden file (or its sha256 if it is large) and tag `v1.0`.

**Deliverable.** The first row of BENCHMARKS.md: "v1 baseline".

**Checkpoint.** Replay finishes with zero errors, and two runs produce byte-identical golden files.

Next: [Phase 2: Python and API](phase-2-python-and-api.md)
