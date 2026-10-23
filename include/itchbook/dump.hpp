#pragma once

#include "itchbook/messages.hpp"

#include <cstdint>
#include <ostream>

namespace itchbook {

// Prints each decoded message as one line:
//   09:30:00.000000000 A locate=13 ref=42 side=B shares=100 stock=AAPL price=123.4500
// The first three columns (time, type, stock locate) are the same for every type.
class DumpHandler : public Handler {
 public:
  // type 0 prints every type. limit 0 means no limit.
  DumpHandler(std::ostream& out, char type, std::uint64_t limit);

  void on_system_event(const SystemEvent& m) override;
  void on_stock_directory(const StockDirectory& m) override;
  void on_add(const AddOrder& m) override;
  void on_execute(const OrderExecuted& m) override;
  void on_execute_with_price(const OrderExecutedWithPrice& m) override;
  void on_cancel(const OrderCancel& m) override;
  void on_delete(const OrderDelete& m) override;
  void on_replace(const OrderReplace& m) override;

  bool done() const override;
  std::uint64_t printed() const { return printed_; }

 private:
  // Returns false if this message is filtered out. Otherwise prints the
  // shared columns and counts the line.
  bool start(const MessageHeader& h);

  std::ostream& out_;
  char type_;
  std::uint64_t limit_;
  std::uint64_t printed_ = 0;
};

}  // namespace itchbook
