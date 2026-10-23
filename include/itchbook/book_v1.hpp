#pragma once

#include "itchbook/messages.hpp"
#include "itchbook/symbols.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

namespace itchbook {

// One price level: the total shares resting at that price.
struct Level {
  std::uint32_t price;
  std::uint64_t shares;

  bool operator==(const Level&) const = default;
};

// The best levels on each side of one stock's book, best first.
struct BookTop {
  std::vector<Level> bids;  // highest price first
  std::vector<Level> asks;  // lowest price first
};

// Where the best bid sits against the best ask. Locked: the same price.
// Crossed: the bid is higher. A book with an empty side is Normal.
enum class BookState { Normal, Locked, Crossed };

// Order book v1: simple and correct, not fast. Keeps every live order by
// reference number, and every price level for every stock.
//
// Invariants after each message:
// - a level's shares equal the sum of the shares of its live orders
// - no level and no order has 0 shares
// Debug builds assert that no message takes more shares than an order or
// level holds, since unsigned subtraction would wrap instead of going negative.
class BookV1 : public Handler {
 public:
  void on_stock_directory(const StockDirectory& m) override;
  void on_add(const AddOrder& m) override;
  void on_execute(const OrderExecuted& m) override;
  void on_execute_with_price(const OrderExecutedWithPrice& m) override;
  void on_cancel(const OrderCancel& m) override;
  void on_delete(const OrderDelete& m) override;
  void on_replace(const OrderReplace& m) override;

  // Up to depth levels per side for one stock. Empty if the stock has no orders.
  BookTop top(std::uint16_t locate, std::size_t depth) const;

  BookState state(std::uint16_t locate) const;

  // Stock names from R messages, so callers can find a stock's locate.
  const SymbolDirectory& symbols() const { return symbols_; }

  std::size_t order_count() const { return orders_.size(); }

  // Price levels across every stock and both sides.
  std::size_t level_count() const;

  // The most orders that were live at once.
  std::size_t peak_order_count() const { return peak_order_count_; }

  // Messages that could not be applied: a reference number that is not live,
  // or an add that reuses one that is. A full-day replay should end at 0.
  std::uint64_t errors() const { return errors_; }

 private:
  struct Order {
    std::uint16_t locate;
    Side side;
    std::uint32_t price;
    std::uint32_t shares;
  };

  struct Book {
    std::map<std::uint32_t, std::uint64_t, std::greater<>> bids;  // price -> total shares, best first
    std::map<std::uint32_t, std::uint64_t> asks;                   // price -> total shares, best first
  };

  void add_order(std::uint64_t ref, const Order& order);
  void reduce_order(std::uint64_t ref, std::uint32_t shares);
  void remove_from_level(const Order& order, std::uint32_t shares);

  std::unordered_map<std::uint64_t, Order> orders_;  // order reference -> order
  std::unordered_map<std::uint16_t, Book> books_;    // stock locate -> book
  SymbolDirectory symbols_;
  std::size_t peak_order_count_ = 0;
  std::uint64_t errors_ = 0;
};

}  // namespace itchbook
