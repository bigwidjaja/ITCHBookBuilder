#pragma once

#include "itchbook/book_v1.hpp"
#include "itchbook/messages.hpp"
#include "itchbook/symbols.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

namespace itchbook {

// Measures the shape of a day's data before any book is built.
// Every message is counted by type, by stock locate, and by second of the day.
// A, F and U messages track the largest order reference number. Book messages
// also go to a BookV1, which names the symbols and counts live orders.
class StatsHandler : public Handler {
 public:
  struct SymbolCount {
    std::uint16_t locate;
    std::string_view symbol;
    std::uint64_t messages;
  };

  struct SecondCount {
    std::uint64_t second;  // seconds since midnight
    std::uint64_t messages;
  };

  StatsHandler();

  void on_message(const MessageHeader& h) override;
  void on_stock_directory(const StockDirectory& m) override;
  void on_add(const AddOrder& m) override;
  void on_execute(const OrderExecuted& m) override;
  void on_execute_with_price(const OrderExecutedWithPrice& m) override;
  void on_cancel(const OrderCancel& m) override;
  void on_delete(const OrderDelete& m) override;
  void on_replace(const OrderReplace& m) override;

  std::uint64_t messages() const { return messages_; }
  const std::array<std::uint64_t, 256>& by_type() const { return by_type_; }
  std::uint64_t max_order_ref() const { return max_order_ref_; }
  const SymbolDirectory& symbols() const { return book_.symbols(); }
  std::size_t peak_order_count() const { return book_.peak_order_count(); }

  // The n locates with the most messages, most first. Ties go to the lower
  // locate. Locate 0 (system messages, not a stock) is left out.
  std::vector<SymbolCount> top_symbols(std::size_t n) const;

  // The second with the most messages. The earliest one wins a tie.
  SecondCount busiest_second() const;

  void print(std::ostream& out) const;

 private:
  std::uint64_t messages_ = 0;
  std::array<std::uint64_t, 256> by_type_{};
  std::vector<std::uint64_t> by_locate_;  // one slot per possible uint16_t locate
  std::vector<std::uint64_t> by_second_;  // one slot per second of the day
  std::uint64_t max_order_ref_ = 0;
  BookV1 book_;
};

}  // namespace itchbook
