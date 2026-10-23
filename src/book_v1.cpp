#include "itchbook/book_v1.hpp"

#include <algorithm>
#include <cassert>

namespace itchbook {

namespace {

// Bids and asks are different map types because their comparators differ,
// so these helpers are templates: the compiler writes one copy for each.

// Takes shares off one level and erases the level when it reaches 0.
template <typename Levels>
void subtract(Levels& levels, std::uint32_t price, std::uint64_t shares) {
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

// Copies the first depth levels. The map is already sorted best first.
template <typename Levels>
std::vector<Level> first_levels(const Levels& levels, std::size_t depth) {
  std::vector<Level> out;
  for (const auto& [price, shares] : levels) {
    if (out.size() == depth) {
      break;
    }
    out.push_back({price, shares});
  }
  return out;
}

}  // namespace

void BookV1::add_order(std::uint64_t ref, const Order& order) {
  const auto [it, inserted] = orders_.emplace(ref, order);
  if (!inserted) {
    ++errors_;  // ref is already live; adding its shares again would double count
    return;
  }
  peak_order_count_ = std::max(peak_order_count_, orders_.size());
  // books_[locate] creates an empty book the first time a stock is seen.
  // Likewise bids[price] starts a new level at 0 before += adds to it.
  Book& book = books_[order.locate];
  if (order.side == Side::Buy) {
    book.bids[order.price] += order.shares;
  } else {
    book.asks[order.price] += order.shares;
  }
}

void BookV1::remove_from_level(const Order& order, std::uint32_t shares) {
  Book& book = books_[order.locate];
  if (order.side == Side::Buy) {
    subtract(book.bids, order.price, shares);
  } else {
    subtract(book.asks, order.price, shares);
  }
}

void BookV1::reduce_order(std::uint64_t ref, std::uint32_t shares) {
  const auto it = orders_.find(ref);
  if (it == orders_.end()) {
    ++errors_;
    return;
  }
  // A reference, not a copy: the subtraction below must change the stored order.
  Order& order = it->second;
  assert(shares <= order.shares);
  remove_from_level(order, shares);
  order.shares -= shares;
  if (order.shares == 0) {
    orders_.erase(it);
  }
}

void BookV1::on_stock_directory(const StockDirectory& m) {
  symbols_.add(m.header.stock_locate, m.stock);
}

void BookV1::on_add(const AddOrder& m) {
  add_order(m.order_ref, {m.header.stock_locate, m.side, m.price, m.shares});
}

void BookV1::on_execute(const OrderExecuted& m) {
  reduce_order(m.order_ref, m.executed_shares);
}

// The shares leave the order's own price level. The execution price is only
// what the trade printed at; it is not where the order was resting.
void BookV1::on_execute_with_price(const OrderExecutedWithPrice& m) {
  reduce_order(m.order_ref, m.executed_shares);
}

void BookV1::on_cancel(const OrderCancel& m) {
  reduce_order(m.order_ref, m.cancelled_shares);
}

void BookV1::on_delete(const OrderDelete& m) {
  const auto it = orders_.find(m.order_ref);
  if (it == orders_.end()) {
    ++errors_;
    return;
  }
  remove_from_level(it->second, it->second.shares);
  orders_.erase(it);
}

// U carries no locate or side, so both come from the old order.
void BookV1::on_replace(const OrderReplace& m) {
  const auto it = orders_.find(m.original_order_ref);
  if (it == orders_.end()) {
    ++errors_;
    return;
  }
  // A copy, not a reference: erase() destroys the stored order, and a
  // reference to it would point at freed memory.
  const Order old = it->second;
  remove_from_level(old, old.shares);
  orders_.erase(it);
  add_order(m.new_order_ref, {old.locate, old.side, m.price, m.shares});
}

BookTop BookV1::top(std::uint16_t locate, std::size_t depth) const {
  const auto it = books_.find(locate);
  if (it == books_.end()) {
    return {};
  }
  return {first_levels(it->second.bids, depth), first_levels(it->second.asks, depth)};
}

BookState BookV1::state(std::uint16_t locate) const {
  const auto it = books_.find(locate);
  if (it == books_.end() || it->second.bids.empty() || it->second.asks.empty()) {
    return BookState::Normal;
  }
  // begin() is the best level on each side: the maps are sorted best first.
  const std::uint32_t best_bid = it->second.bids.begin()->first;
  const std::uint32_t best_ask = it->second.asks.begin()->first;
  if (best_bid == best_ask) {
    return BookState::Locked;
  }
  return best_bid > best_ask ? BookState::Crossed : BookState::Normal;
}

}  // namespace itchbook
