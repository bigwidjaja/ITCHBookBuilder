#pragma once

#include "itchbook/book_v1.hpp"
#include "itchbook/flat_map.hpp"
#include "itchbook/messages.hpp"
#include "itchbook/symbols.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
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

// The v2 book the tools use by default.
using BookV2 = BasicBookV2<StdOrderStore>;

}  // namespace itchbook
