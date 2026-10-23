#include "itchbook/stats.hpp"

#include "itchbook/format.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace itchbook {

namespace {

constexpr std::uint64_t ns_per_second = 1'000'000'000;
constexpr std::size_t seconds_per_day = 24 * 60 * 60;

}  // namespace

// Sizing both vectors up front means any uint16_t locate is a valid index,
// and so is any second of the day, with no resizing while counting.
StatsHandler::StatsHandler() : by_locate_(65536), by_second_(seconds_per_day) {}

void StatsHandler::on_message(const MessageHeader& h) {
  ++messages_;
  ++by_type_[static_cast<unsigned char>(h.type)];
  ++by_locate_[h.stock_locate];
  const std::uint64_t second = h.timestamp / ns_per_second;
  if (second < by_second_.size()) {
    ++by_second_[second];
  }
}

void StatsHandler::on_stock_directory(const StockDirectory& m) {
  book_.on_stock_directory(m);
}

void StatsHandler::on_add(const AddOrder& m) {
  max_order_ref_ = std::max(max_order_ref_, m.order_ref);
  book_.on_add(m);
}

void StatsHandler::on_execute(const OrderExecuted& m) {
  book_.on_execute(m);
}

void StatsHandler::on_execute_with_price(const OrderExecutedWithPrice& m) {
  book_.on_execute_with_price(m);
}

void StatsHandler::on_cancel(const OrderCancel& m) {
  book_.on_cancel(m);
}

void StatsHandler::on_delete(const OrderDelete& m) {
  book_.on_delete(m);
}

// A replace creates a new order, so its new reference number counts too.
void StatsHandler::on_replace(const OrderReplace& m) {
  max_order_ref_ = std::max(max_order_ref_, m.new_order_ref);
  book_.on_replace(m);
}

std::vector<StatsHandler::SymbolCount> StatsHandler::top_symbols(std::size_t n) const {
  std::vector<SymbolCount> all;
  for (std::size_t locate = 1; locate < by_locate_.size(); ++locate) {
    if (by_locate_[locate] != 0) {
      const auto l = static_cast<std::uint16_t>(locate);
      all.push_back({l, symbols().symbol(l), by_locate_[locate]});
    }
  }

  // partial_sort only puts the first `top` in order, which is all we print.
  // The lambda says what "comes first" means: more messages, then lower locate.
  const std::size_t top = std::min(n, all.size());
  std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(top), all.end(),
                    [](const SymbolCount& a, const SymbolCount& b) {
                      if (a.messages != b.messages) {
                        return a.messages > b.messages;
                      }
                      return a.locate < b.locate;
                    });
  all.resize(top);
  return all;
}

StatsHandler::SecondCount StatsHandler::busiest_second() const {
  // max_element returns the first largest, so the earliest second wins a tie.
  const auto it = std::max_element(by_second_.begin(), by_second_.end());
  return {static_cast<std::uint64_t>(it - by_second_.begin()), *it};
}

void StatsHandler::print(std::ostream& out) const {
  const SecondCount busiest = busiest_second();
  // format_timestamp gives HH:MM:SS.nnnnnnnnn; the first 8 characters are HH:MM:SS.
  const std::string busiest_time = format_timestamp(busiest.second * ns_per_second).substr(0, 8);

  out << "messages: " << messages_ << '\n';
  out << "symbols: " << symbols().size() << '\n';
  out << "largest order ref: " << max_order_ref_ << '\n';
  out << "peak live orders: " << peak_order_count() << '\n';
  out << "busiest second: " << busiest_time << " (" << busiest.messages << " messages)\n";

  std::vector<std::pair<char, std::uint64_t>> types;
  for (std::size_t type = 0; type < by_type_.size(); ++type) {
    if (by_type_[type] != 0) {
      types.emplace_back(static_cast<char>(type), by_type_[type]);
    }
  }
  std::sort(types.begin(), types.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  out << "\nmessages per type:\n";
  for (const auto& [type, count] : types) {
    out << std::format("  {} {:>12}\n", type, count);
  }

  out << "\ntop 10 symbols by messages:\n";
  const std::vector<SymbolCount> top = top_symbols(10);
  for (std::size_t i = 0; i < top.size(); ++i) {
    out << std::format("  {:>2}. {:<8} {:>12}\n", i + 1, top[i].symbol, top[i].messages);
  }
}

}  // namespace itchbook
