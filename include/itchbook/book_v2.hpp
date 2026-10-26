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

// One live order.
struct OrderV2 {
  std::uint16_t locate;
  Side side;
  std::uint32_t price;
  std::uint32_t shares;
};

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

template <class OrderStore>
class BasicBookV2 : public Handler {
 public:
  void on_stock_directory(const StockDirectory& m) override {
    symbols_.add(m.header.stock_locate, m.stock);
  }

  void on_add(const AddOrder& m) override {
    add_order(m.order_ref, {m.header.stock_locate, m.side, m.price, m.shares});
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
    remove_from_level(*order, order->shares);
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
    remove_from_level(old, old.shares);
    orders_.erase(m.original_order_ref);
    add_order(m.new_order_ref, {old.locate, old.side, m.price, m.shares});
  }

  BookTop top(std::uint16_t locate, std::size_t depth) const {
    const auto it = books_.find(locate);
    if (it == books_.end()) {
      return {};
    }
    return {first_levels(it->second.bids, depth), first_levels(it->second.asks, depth)};
  }

  BookState state(std::uint16_t locate) const {
    const auto it = books_.find(locate);
    if (it == books_.end() || it->second.bids.empty() || it->second.asks.empty()) {
      return BookState::Normal;
    }
    const std::uint32_t best_bid = it->second.bids.begin()->first;
    const std::uint32_t best_ask = it->second.asks.begin()->first;
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
    for (const auto& [locate, book] : books_) {
      levels += book.bids.size() + book.asks.size();
    }
    return levels;
  }

 private:
  struct Book {
    std::map<std::uint32_t, std::uint64_t, std::greater<>> bids;  // price -> total shares, best first
    std::map<std::uint32_t, std::uint64_t> asks;                   // price -> total shares, best first
  };

  void add_order(std::uint64_t ref, const OrderV2& order) {
    if (!orders_.insert(ref, order)) {
      ++errors_;  // ref is already live; adding its shares again would double count
      return;
    }
    peak_order_count_ = std::max(peak_order_count_, orders_.size());
    Book& book = books_[order.locate];
    if (order.side == Side::Buy) {
      book.bids[order.price] += order.shares;
    } else {
      book.asks[order.price] += order.shares;
    }
  }

  void reduce_order(std::uint64_t ref, std::uint32_t shares) {
    OrderV2* order = orders_.find(ref);
    if (order == nullptr) {
      ++errors_;
      return;
    }
    assert(shares <= order->shares);
    remove_from_level(*order, shares);
    order->shares -= shares;
    if (order->shares == 0) {
      orders_.erase(ref);
    }
  }

  void remove_from_level(const OrderV2& order, std::uint32_t shares) {
    Book& book = books_[order.locate];
    if (order.side == Side::Buy) {
      subtract(book.bids, order.price, shares);
    } else {
      subtract(book.asks, order.price, shares);
    }
  }

  template <typename Levels>
  static void subtract(Levels& levels, std::uint32_t price, std::uint64_t shares) {
    const auto it = levels.find(price);
    assert(it != levels.end());
    if (it == levels.end()) {
      return;  // only possible if the invariants are already broken
    }
    assert(shares <= it->second);
    it->second -= shares;
    if (it->second == 0) {
      levels.erase(it);
    }
  }

  template <typename Levels>
  static std::vector<Level> first_levels(const Levels& levels, std::size_t depth) {
    std::vector<Level> out;
    for (const auto& [price, shares] : levels) {
      if (out.size() == depth) {
        break;
      }
      out.push_back({price, shares});
    }
    return out;
  }

  OrderStore orders_;                              // order reference -> order
  std::unordered_map<std::uint16_t, Book> books_;  // stock locate -> book
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

}  // namespace itchbook
