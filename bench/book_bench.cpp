// Microbenchmarks for BookV1's most common operations: add, execute and
// delete (A, E and D are 88% of a real day). Orders are synthetic and cluster
// near the best price the way real ones do.
//
// Each benchmark's argument is how many orders already rest in the book. On
// the real day one BookV1 holds up to 1.9M live orders across all stocks.
//
// Build: cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DITCHBOOK_BUILD_BENCH=ON
// Run:   build-rel/itchbook_bench

#include "itchbook/book_v1.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {

constexpr std::uint32_t mid = 1'000'000;  // $100.0000
constexpr std::uint32_t tick = 100;       // $0.01
constexpr std::size_t batch = 1024;       // operations per timed batch

// A buy below or a sell above $100.00. The distance is 1 tick plus a geometric
// number of ticks, so most orders sit within a few cents of the best price.
itchbook::AddOrder make_add(std::mt19937_64& rng, std::uint64_t ref) {
  std::geometric_distribution<std::uint32_t> extra_ticks(0.3);
  std::bernoulli_distribution buy(0.5);
  const bool is_buy = buy(rng);
  const std::uint32_t offset = (1 + extra_ticks(rng)) * tick;

  itchbook::AddOrder m{};
  m.header = {'A', 1, 0, 0};
  m.order_ref = ref;
  m.side = is_buy ? itchbook::Side::Buy : itchbook::Side::Sell;
  m.shares = 1'000'000'000;  // large, so executes in BM_Execute never empty an order
  m.price = is_buy ? mid - offset : mid + offset;
  return m;
}

itchbook::OrderDelete make_delete(std::uint64_t ref) {
  itchbook::OrderDelete m{};
  m.header = {'D', 1, 0, 0};
  m.order_ref = ref;
  return m;
}

// Fills book with orders 1..n.
void fill(itchbook::BookV1& book, std::size_t n, std::mt19937_64& rng) {
  for (std::uint64_t ref = 1; ref <= n; ++ref) {
    book.on_add(make_add(rng, ref));
  }
}

// One batch of adds on top of the resting orders. The same batch is deleted
// again between iterations, with the timer paused, so every iteration starts
// from the same book.
void BM_Add(benchmark::State& state) {
  const auto resting = static_cast<std::size_t>(state.range(0));
  std::mt19937_64 rng(42);
  itchbook::BookV1 book;
  fill(book, resting, rng);
  std::vector<itchbook::AddOrder> adds;
  for (std::size_t i = 1; i <= batch; ++i) {
    adds.push_back(make_add(rng, resting + i));
  }

  for (auto _ : state) {
    for (const auto& m : adds) {
      book.on_add(m);
    }
    benchmark::DoNotOptimize(book);
    benchmark::ClobberMemory();

    state.PauseTiming();
    for (const auto& m : adds) {
      book.on_delete(make_delete(m.order_ref));
    }
    state.ResumeTiming();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * batch));
}

// One batch of deletes of orders added just before, with the timer paused.
void BM_Delete(benchmark::State& state) {
  const auto resting = static_cast<std::size_t>(state.range(0));
  std::mt19937_64 rng(42);
  itchbook::BookV1 book;
  fill(book, resting, rng);
  std::vector<itchbook::AddOrder> adds;
  for (std::size_t i = 1; i <= batch; ++i) {
    adds.push_back(make_add(rng, resting + i));
  }

  for (auto _ : state) {
    state.PauseTiming();
    for (const auto& m : adds) {
      book.on_add(m);
    }
    state.ResumeTiming();

    for (const auto& m : adds) {
      book.on_delete(make_delete(m.order_ref));
    }
    benchmark::DoNotOptimize(book);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * batch));
}

// Executes one share of a random resting order. The order and its level keep
// existing, so no pause is needed.
void BM_Execute(benchmark::State& state) {
  const auto resting = static_cast<std::size_t>(state.range(0));
  std::mt19937_64 rng(42);
  itchbook::BookV1 book;
  fill(book, resting, rng);
  std::vector<itchbook::OrderExecuted> executes(batch);
  std::uniform_int_distribution<std::uint64_t> any_ref(1, resting);
  for (auto& m : executes) {
    m.header = {'E', 1, 0, 0};
    m.order_ref = any_ref(rng);
    m.executed_shares = 1;
  }

  for (auto _ : state) {
    for (const auto& m : executes) {
      book.on_execute(m);
    }
    benchmark::DoNotOptimize(book);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * batch));
}

// The real day's most common pattern: an order added and soon deleted. Each
// step adds one order and deletes the oldest, so the book size stays fixed.
void BM_AddThenDeleteOldest(benchmark::State& state) {
  const auto resting = static_cast<std::size_t>(state.range(0));
  std::mt19937_64 rng(42);
  itchbook::BookV1 book;
  fill(book, resting, rng);
  std::vector<itchbook::AddOrder> adds;
  for (std::size_t i = 0; i < 4096; ++i) {
    adds.push_back(make_add(rng, 0));
  }

  std::uint64_t next = resting + 1;
  std::size_t i = 0;
  for (auto _ : state) {
    itchbook::AddOrder m = adds[i++ % adds.size()];
    m.order_ref = next;
    book.on_add(m);
    book.on_delete(make_delete(next - resting));
    ++next;
    benchmark::DoNotOptimize(book);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * 2));
}

}  // namespace

// 1K and 16K resting orders is one stock's book; 1M is the whole market.
BENCHMARK(BM_Add)->Arg(1 << 10)->Arg(1 << 14)->Arg(1 << 20);
BENCHMARK(BM_Delete)->Arg(1 << 10)->Arg(1 << 14)->Arg(1 << 20);
BENCHMARK(BM_Execute)->Arg(1 << 10)->Arg(1 << 14)->Arg(1 << 20);
BENCHMARK(BM_AddThenDeleteOldest)->Arg(1 << 10)->Arg(1 << 14)->Arg(1 << 20);

BENCHMARK_MAIN();
