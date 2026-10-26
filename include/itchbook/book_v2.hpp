#pragma once

#include "itchbook/book_v1.hpp"
#include "itchbook/flat_map.hpp"
#include "itchbook/messages.hpp"
#include "itchbook/symbols.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace itchbook {

// Order book v2: where Phase 4 makes the book fast. BookV1 stays frozen as the
// trusted reference; v2 must give exactly its answers (the golden file, and
// `itchbook diff`, which compares the two after every message).
//
// The public interface is the same as BookV1's, so the replay tools work with
// either. Each Step 10 variant is a different OrderStore: the map from order
// reference to order. A store provides
//   OrderV2* find(std::uint64_t ref);              nullptr if absent
//   bool insert(std::uint64_t ref, const OrderV2&); false if ref is live
//   bool erase(std::uint64_t ref);                 false if absent
//   std::size_t size() const;
//   void reserve(std::size_t n);
// and BasicBookV2 is a template over it, so the compiler builds one book per
// store with no virtual calls between them.

// The most orders live at once on 2019-12-30, measured in Step 4. A full-day
// replay reserves room for this many up front, so the order table never has
// to grow (and rehash every live order) partway through the day.
inline constexpr std::size_t day_peak_live_orders = 1'924'078;

// One live order, kept small because there are up to 1.9M of them (Step 11
// task 5). It has no price: its level in the pool has that. Fields go largest
// first so none needs padding before it: 4 + 4 + 2 + 1 = 11 bytes, rounded up
// to 12 so an array of them keeps each uint32_t 4-byte aligned. The earlier
// {locate, side, price, shares, level} was 16: a padding byte after side, and
// a price no one read.
struct OrderV2 {
  std::uint32_t shares;
  std::uint32_t level;  // pool index of its price level
  std::uint16_t locate;
  Side side;
};
static_assert(sizeof(OrderV2) == 12, "OrderV2 grew; check field order and types");

// std::unordered_map behind the OrderStore interface: v1's order table.
class StdOrderStore {
 public:
  OrderV2* find(std::uint64_t ref) {
    const auto it = map_.find(ref);
    return it == map_.end() ? nullptr : &it->second;
  }
  bool insert(std::uint64_t ref, const OrderV2& order) { return map_.emplace(ref, order).second; }
  bool erase(std::uint64_t ref) { return map_.erase(ref) == 1; }
  std::size_t size() const { return map_.size(); }
  void reserve(std::size_t n) { map_.reserve(n); }

 private:
  std::unordered_map<std::uint64_t, OrderV2> map_;
};

// The largest order reference number on 2019-12-30, measured in Step 3.
inline constexpr std::uint64_t day_largest_order_ref = 308'631'072;

// Orders in one array indexed directly by reference number (Step 10 task 5):
// order 8913 is slots_[8913], with no hashing and no probing.
//
// The cost is memory: the array spans every reference number up to the
// largest, used or not, so it takes sizeof(OrderV2) = 12 bytes per number,
// about 3.7 GB for the full day. calloc helps only at first: it returns pages
// the OS has not handed out yet, made real (zero-filled) when first written.
// Over a day the references sweep across the whole range, so most pages end
// up written.
//
// A slot with stock locate 0 is empty: locates start at 1, and 0 only appears
// on system messages, never on an order. (The first version marked empty
// slots with 0 shares, and the typed book tests caught it: the book sets an
// order's shares to 0 before erasing it, so erase then found nothing.)
//
// It owns raw memory from calloc, so copying is deleted and moving hands the
// array over, as with MappedFile.
class DirectOrderStore {
 public:
  DirectOrderStore() = default;
  ~DirectOrderStore() { std::free(slots_); }

  DirectOrderStore(const DirectOrderStore&) = delete;
  DirectOrderStore& operator=(const DirectOrderStore&) = delete;
  DirectOrderStore(DirectOrderStore&& other) noexcept
      : slots_(std::exchange(other.slots_, nullptr)),
        capacity_(std::exchange(other.capacity_, 0)),
        size_(std::exchange(other.size_, 0)) {}
  DirectOrderStore& operator=(DirectOrderStore&& other) noexcept {
    if (this != &other) {
      std::free(slots_);
      slots_ = std::exchange(other.slots_, nullptr);
      capacity_ = std::exchange(other.capacity_, 0);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }

  OrderV2* find(std::uint64_t ref) {
    return ref < capacity_ && slots_[ref].locate != 0 ? &slots_[ref] : nullptr;
  }

  bool insert(std::uint64_t ref, const OrderV2& order) {
    if (ref >= capacity_) {
      grow(ref + 1);
    }
    if (slots_[ref].locate != 0) {
      return false;
    }
    slots_[ref] = order;
    ++size_;
    return true;
  }

  bool erase(std::uint64_t ref) {
    OrderV2* order = find(ref);
    if (order == nullptr) {
      return false;
    }
    *order = OrderV2{};
    --size_;
    return true;
  }

  std::size_t size() const { return size_; }

  // n counts live orders, but this array is sized by the largest reference
  // number, so there is nothing useful to reserve here.
  void reserve(std::size_t) {}

 private:
  // The first allocation covers the whole day's range at once: calloc'd pages
  // cost nothing until written, and growing later would copy gigabytes.
  void grow(std::uint64_t needed) {
    const std::uint64_t capacity = std::max({needed, capacity_ * 2, day_largest_order_ref + 1});
    auto* slots = static_cast<OrderV2*>(std::calloc(capacity, sizeof(OrderV2)));
    if (slots == nullptr) {
      throw std::bad_alloc();
    }
    if (slots_ != nullptr) {
      std::memcpy(slots, slots_, capacity_ * sizeof(OrderV2));
      std::free(slots_);
    }
    slots_ = slots;
    capacity_ = capacity;
  }

  OrderV2* slots_ = nullptr;
  std::uint64_t capacity_ = 0;
  std::size_t size_ = 0;
};

// How a side's price levels are searched (Step 11 task 3). Levels are sorted
// worst to best, so the best sits at the back; better(a, b) says whether price
// a beats price b on this side. find returns where `price` is, or where it
// would be inserted, as {index, found}.

// Walks back from the best price. Most updates land a few levels from the
// top, where this stops after a step or two.
struct ScanFromBest {
  template <class Levels, class Better>
  static std::pair<std::size_t, bool> find(const Levels& levels, std::uint32_t price,
                                           Better better) {
    std::size_t i = levels.size();
    while (i > 0 && better(levels[i - 1].price, price)) {
      --i;
    }
    if (i > 0 && levels[i - 1].price == price) {
      return {i - 1, true};
    }
    return {i, false};
  }
};

// Binary search: about log2(levels) steps wherever the price is, but each
// step jumps around the vector instead of walking it.
struct BinarySearch {
  template <class Levels, class Better>
  static std::pair<std::size_t, bool> find(const Levels& levels, std::uint32_t price,
                                           Better better) {
    // lower_bound wants "is this level before price in the sort order", which
    // here means "is this level worse than price".
    const auto it = std::lower_bound(
        levels.begin(), levels.end(), price,
        [&](const auto& level, std::uint32_t p) { return better(p, level.price); });
    return {static_cast<std::size_t>(it - levels.begin()), it != levels.end() && it->price == price};
  }
};

template <class OrderStore, class LevelSearch = ScanFromBest>
class BasicBookV2 : public Handler {
 public:
  void on_stock_directory(const StockDirectory& m) override {
    symbols_.add(m.header.stock_locate, m.stock);
  }

  void on_add(const AddOrder& m) override {
    add_order(m.order_ref, m.header.stock_locate, m.side, m.price, m.shares);
  }

  void on_execute(const OrderExecuted& m) override { reduce_order(m.order_ref, m.executed_shares); }

  // The shares leave the order's own level, not the execution price.
  void on_execute_with_price(const OrderExecutedWithPrice& m) override {
    reduce_order(m.order_ref, m.executed_shares);
  }

  void on_cancel(const OrderCancel& m) override { reduce_order(m.order_ref, m.cancelled_shares); }

  void on_delete(const OrderDelete& m) override {
    OrderV2* order = orders_.find(m.order_ref);
    if (order == nullptr) {
      ++errors_;
      return;
    }
    remove_shares(*order, order->shares);
    orders_.erase(m.order_ref);
  }

  // U carries no locate or side, so both come from the old order.
  void on_replace(const OrderReplace& m) override {
    OrderV2* found = orders_.find(m.original_order_ref);
    if (found == nullptr) {
      ++errors_;
      return;
    }
    // A copy: erase() may move or free the stored order.
    const OrderV2 old = *found;
    remove_shares(old, old.shares);
    orders_.erase(m.original_order_ref);
    add_order(m.new_order_ref, old.locate, old.side, m.price, m.shares);
  }

  BookTop top(std::uint16_t locate, std::size_t depth) const {
    const Book* book = find_book(locate);
    if (book == nullptr) {
      return {};
    }
    return {first_levels(book->bids, depth), first_levels(book->asks, depth)};
  }

  BookState state(std::uint16_t locate) const {
    const Book* book = find_book(locate);
    if (book == nullptr || book->bids.empty() || book->asks.empty()) {
      return BookState::Normal;
    }
    const std::uint32_t best_bid = book->bids.back().price;  // the best is at the back
    const std::uint32_t best_ask = book->asks.back().price;
    if (best_bid == best_ask) {
      return BookState::Locked;
    }
    return best_bid > best_ask ? BookState::Crossed : BookState::Normal;
  }

  // Makes room for n live orders at once. Not done by default: tests and
  // tools build many small books, and a table sized for a whole day is large.
  void reserve_orders(std::size_t n) { orders_.reserve(n); }

  const SymbolDirectory& symbols() const { return symbols_; }
  std::size_t order_count() const { return orders_.size(); }
  std::size_t peak_order_count() const { return peak_order_count_; }
  std::uint64_t errors() const { return errors_; }

  std::size_t level_count() const {
    std::size_t levels = 0;
    for (const Book& book : books_) {
      levels += book.bids.size() + book.asks.size();
    }
    return levels;
  }

 private:
  // A price level's shares live in one pool shared by every stock
  // (Step 11 task 4). The sorted sides hold only {price, pool index}, and each
  // order keeps its level's pool index, so execute, cancel and delete reach
  // their level directly. Searching is left for when a level is created (an
  // add at a new price) or emptied (it must leave its sorted side). Emptied
  // pool slots go on a free list for reuse.
  struct PoolLevel {
    std::uint64_t shares;
    std::uint32_t price;  // so an emptied level can be found on its side
  };

  struct LevelRef {
    std::uint32_t price;  // copied here so searching a side stays in one array
    std::uint32_t index;  // into pool_
  };

  // Each side is sorted worst to best, so the best level is at the back: bids
  // by rising price, asks by falling price. Most updates are near the best
  // price, so inserts and erases there move few elements.
  struct Book {
    std::vector<LevelRef> bids;
    std::vector<LevelRef> asks;
  };

  // "a is a better price than b" on each side.
  using BidBetter = std::greater<>;
  using AskBetter = std::less<>;

  // books_ is indexed by stock locate: locates are small and have no gaps
  // (1 to 8,906 on 2019-12-30), so an index replaces a hash lookup.
  Book& book_for(std::uint16_t locate) {
    if (locate >= books_.size()) {
      books_.resize(std::size_t{locate} + 1);
    }
    return books_[locate];
  }

  const Book* find_book(std::uint16_t locate) const {
    return locate < books_.size() ? &books_[locate] : nullptr;
  }

  void add_order(std::uint64_t ref, std::uint16_t locate, Side side, std::uint32_t price,
                 std::uint32_t shares) {
    // The level first, so the order can be stored with its pool index in one
    // insert. A duplicate reference (never seen in real data) undoes it.
    const std::uint32_t level = add_to_level(locate, side, price, shares);
    const OrderV2 order{.shares = shares, .level = level, .locate = locate, .side = side};
    if (!orders_.insert(ref, order)) {
      ++errors_;  // ref is already live; its shares must not count twice
      remove_shares(order, shares);
      return;
    }
    peak_order_count_ = std::max(peak_order_count_, orders_.size());
  }

  void reduce_order(std::uint64_t ref, std::uint32_t shares) {
    OrderV2* order = orders_.find(ref);
    if (order == nullptr) {
      ++errors_;
      return;
    }
    assert(shares <= order->shares);
    remove_shares(*order, shares);
    order->shares -= shares;
    if (order->shares == 0) {
      orders_.erase(ref);
    }
  }

  // Adds shares at price on one side, creating the level if it is new.
  // Returns the level's pool index.
  std::uint32_t add_to_level(std::uint16_t locate, Side side, std::uint32_t price,
                             std::uint64_t shares) {
    Book& book = book_for(locate);
    return side == Side::Buy ? add_to(book.bids, price, shares, BidBetter{})
                             : add_to(book.asks, price, shares, AskBetter{});
  }

  template <class Better>
  std::uint32_t add_to(std::vector<LevelRef>& side, std::uint32_t price, std::uint64_t shares,
                       Better better) {
    const auto [i, found] = LevelSearch::find(side, price, better);
    if (found) {
      pool_[side[i].index].shares += shares;
      return side[i].index;
    }
    const std::uint32_t index = new_level(price, shares);
    side.insert(side.begin() + static_cast<std::ptrdiff_t>(i), LevelRef{price, index});
    return index;
  }

  // Takes shares off an order's level, straight through its pool index. Only
  // an emptied level needs a search, to leave its sorted side.
  void remove_shares(const OrderV2& order, std::uint64_t shares) {
    PoolLevel& level = pool_[order.level];
    assert(shares <= level.shares);
    level.shares -= shares;
    if (level.shares != 0) {
      return;
    }
    Book& book = book_for(order.locate);
    if (order.side == Side::Buy) {
      erase_level(book.bids, level.price, order.level, BidBetter{});
    } else {
      erase_level(book.asks, level.price, order.level, AskBetter{});
    }
    free_levels_.push_back(order.level);
  }

  template <class Better>
  static void erase_level(std::vector<LevelRef>& side, std::uint32_t price, std::uint32_t index,
                          Better better) {
    const auto [i, found] = LevelSearch::find(side, price, better);
    assert(found && side[i].index == index);
    if (found) {
      side.erase(side.begin() + static_cast<std::ptrdiff_t>(i));
    }
    (void)index;  // only read by the assert
  }

  std::uint32_t new_level(std::uint32_t price, std::uint64_t shares) {
    if (!free_levels_.empty()) {
      const std::uint32_t index = free_levels_.back();
      free_levels_.pop_back();
      pool_[index] = PoolLevel{shares, price};
      return index;
    }
    pool_.push_back(PoolLevel{shares, price});
    return static_cast<std::uint32_t>(pool_.size() - 1);
  }

  // The best depth levels, best first: walk from the back.
  std::vector<Level> first_levels(const std::vector<LevelRef>& side, std::size_t depth) const {
    std::vector<Level> out;
    for (auto it = side.rbegin(); it != side.rend() && out.size() < depth; ++it) {
      out.push_back({it->price, pool_[it->index].shares});
    }
    return out;
  }

  OrderStore orders_;                       // order reference -> order
  std::vector<Book> books_;                 // stock locate -> book
  std::vector<PoolLevel> pool_;             // every level of every stock
  std::vector<std::uint32_t> free_levels_;  // pool indexes free for reuse
  SymbolDirectory symbols_;
  std::size_t peak_order_count_ = 0;
  std::uint64_t errors_ = 0;
};

// The v2 book the tools use: orders in a FlatMap (Step 10 task 4).
using BookV2 = BasicBookV2<FlatMap<std::uint64_t, OrderV2>>;

// The same book with std::unordered_map orders, kept so the two stores can be
// compared from one binary (itchbook book --book v2-std).
using BookV2Std = BasicBookV2<StdOrderStore>;

// The direct-array experiment (itchbook book --book v2-direct).
using BookV2Direct = BasicBookV2<DirectOrderStore>;

// BookV2 with binary search instead of scanning from the best price
// (itchbook book --book v2-bsearch).
using BookV2BinarySearch = BasicBookV2<FlatMap<std::uint64_t, OrderV2>, BinarySearch>;

}  // namespace itchbook
