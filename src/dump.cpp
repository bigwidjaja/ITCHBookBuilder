#include "itchbook/dump.hpp"

#include "itchbook/format.hpp"

namespace itchbook {

DumpHandler::DumpHandler(std::ostream& out, char type, std::uint64_t limit)
    : out_(out), type_(type), limit_(limit) {}

bool DumpHandler::start(const MessageHeader& h) {
  if (type_ != 0 && h.type != type_) {
    return false;
  }
  out_ << format_timestamp(h.timestamp) << ' ' << h.type << " locate=" << h.stock_locate;
  ++printed_;
  return true;
}

bool DumpHandler::done() const {
  return limit_ != 0 && printed_ >= limit_;
}

// Each line ends with '\n', not std::endl: endl also flushes the stream,
// which is slow when printing millions of lines.

void DumpHandler::on_system_event(const SystemEvent& m) {
  if (!start(m.header)) return;
  out_ << " event=" << m.event_code << '\n';
}

void DumpHandler::on_stock_directory(const StockDirectory& m) {
  if (!start(m.header)) return;
  out_ << " stock=" << m.stock << " category=" << m.market_category
       << " status=" << m.financial_status << " round_lot=" << m.round_lot_size << '\n';
}

void DumpHandler::on_add(const AddOrder& m) {
  if (!start(m.header)) return;
  out_ << " ref=" << m.order_ref << " side=" << static_cast<char>(m.side)
       << " shares=" << m.shares << " stock=" << m.stock << " price=" << format_price(m.price);
  if (!m.attribution.empty()) {
    out_ << " mpid=" << m.attribution;
  }
  out_ << '\n';
}

void DumpHandler::on_execute(const OrderExecuted& m) {
  if (!start(m.header)) return;
  out_ << " ref=" << m.order_ref << " shares=" << m.executed_shares
       << " match=" << m.match_number << '\n';
}

void DumpHandler::on_execute_with_price(const OrderExecutedWithPrice& m) {
  if (!start(m.header)) return;
  out_ << " ref=" << m.order_ref << " shares=" << m.executed_shares
       << " match=" << m.match_number << " printable=" << (m.printable ? 'Y' : 'N')
       << " price=" << format_price(m.execution_price) << '\n';
}

void DumpHandler::on_cancel(const OrderCancel& m) {
  if (!start(m.header)) return;
  out_ << " ref=" << m.order_ref << " shares=" << m.cancelled_shares << '\n';
}

void DumpHandler::on_delete(const OrderDelete& m) {
  if (!start(m.header)) return;
  out_ << " ref=" << m.order_ref << '\n';
}

void DumpHandler::on_replace(const OrderReplace& m) {
  if (!start(m.header)) return;
  out_ << " old_ref=" << m.original_order_ref << " new_ref=" << m.new_order_ref
       << " shares=" << m.shares << " price=" << format_price(m.price) << '\n';
}

}  // namespace itchbook
