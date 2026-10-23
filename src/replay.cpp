#include "itchbook/replay.hpp"

#include "itchbook/format.hpp"

#include <algorithm>
#include <utility>

namespace itchbook {

void CheckedBook::on_message(const MessageHeader&) {
  ++messages_;
}

// Each book message is applied first, then the stock it touched is checked.

void CheckedBook::on_add(const AddOrder& m) {
  BookV1::on_add(m);
  check(m.header);
}

void CheckedBook::on_execute(const OrderExecuted& m) {
  BookV1::on_execute(m);
  check(m.header);
}

void CheckedBook::on_execute_with_price(const OrderExecutedWithPrice& m) {
  BookV1::on_execute_with_price(m);
  check(m.header);
}

void CheckedBook::on_cancel(const OrderCancel& m) {
  BookV1::on_cancel(m);
  check(m.header);
}

void CheckedBook::on_delete(const OrderDelete& m) {
  BookV1::on_delete(m);
  check(m.header);
}

void CheckedBook::on_replace(const OrderReplace& m) {
  BookV1::on_replace(m);
  check(m.header);
}

void CheckedBook::check(const MessageHeader& h) {
  if (h.timestamp < market_open || h.timestamp >= market_close) {
    return;
  }
  const BookState s = state(h.stock_locate);
  if (s == BookState::Normal) {
    return;
  }
  // try_emplace adds a Problem the first time this stock has one.
  auto [it, first] = problems_.try_emplace(h.stock_locate);
  Problem& p = it->second;
  if (first) {
    p.locate = h.stock_locate;
    p.symbol = symbols().symbol(h.stock_locate);
    p.first_time = h.timestamp;
  }
  p.last_time = h.timestamp;
  if (s == BookState::Locked) {
    ++locked_;
    ++p.locked;
  } else {
    ++crossed_;
    ++p.crossed;
  }
}

std::vector<CheckedBook::Problem> CheckedBook::worst(std::size_t n) const {
  std::vector<Problem> all;
  for (const auto& [locate, problem] : problems_) {
    all.push_back(problem);
  }
  // Sorted fully, not partially: problems_ is an unordered_map, so ties need
  // the locate tie-break to come out the same on every run.
  std::sort(all.begin(), all.end(), [](const Problem& a, const Problem& b) {
    const std::uint64_t a_total = a.locked + a.crossed;
    const std::uint64_t b_total = b.locked + b.crossed;
    if (a_total != b_total) {
      return a_total > b_total;
    }
    return a.locate < b.locate;
  });
  all.resize(std::min(n, all.size()));
  return all;
}

BookAt::BookAt(std::string symbol, std::uint64_t time, std::size_t depth)
    : symbol_(std::move(symbol)), time_(time), depth_(depth) {}

void BookAt::on_message(const MessageHeader& h) {
  if (!snapshot_ && h.timestamp > time_) {
    snapshot_ = top_now();
  }
}

BookTop BookAt::top_now() const {
  const auto l = locate();
  return l ? top(*l, depth_) : BookTop{};
}

BookTop BookAt::result() const {
  return snapshot_ ? *snapshot_ : top_now();
}

GoldenWriter::GoldenWriter(std::ostream& out, std::vector<std::string> symbols, std::size_t depth,
                           std::uint64_t interval)
    : out_(out), symbols_(std::move(symbols)), depth_(depth), ticker_(interval) {
  out_ << "time,symbol";
  for (const char* side : {"bid", "ask"}) {
    for (std::size_t i = 1; i <= depth_; ++i) {
      out_ << ',' << side << "_price_" << i << ',' << side << "_shares_" << i;
    }
  }
  out_ << '\n';
}

void GoldenWriter::on_message(const MessageHeader& h) {
  ticker_.advance(h.timestamp, [this](std::uint64_t time) { write_snapshot(time); });
  CheckedBook::on_message(h);
}

void GoldenWriter::write_snapshot(std::uint64_t time) {
  // HH:MM:SS.nnnnnnnnn; snapshots fall on whole seconds, so keep HH:MM:SS.
  const std::string clock = format_timestamp(time).substr(0, 8);
  for (const std::string& symbol : symbols_) {
    const auto locate = symbols().find(symbol);
    const BookTop t = locate ? top(*locate, depth_) : BookTop{};
    out_ << clock << ',' << symbol;
    for (const std::vector<Level>* side : {&t.bids, &t.asks}) {
      for (std::size_t i = 0; i < depth_; ++i) {
        if (i < side->size()) {
          out_ << ',' << format_price((*side)[i].price) << ',' << (*side)[i].shares;
        } else {
          out_ << ",,";
        }
      }
    }
    out_ << '\n';
    ++rows_;
  }
}

void TopOfBookColumns::add(std::uint64_t t, const BookTop& top) {
  time.push_back(t);
  bid_price.push_back(top.bids.empty() ? 0 : top.bids[0].price);
  bid_shares.push_back(top.bids.empty() ? 0 : top.bids[0].shares);
  ask_price.push_back(top.asks.empty() ? 0 : top.asks[0].price);
  ask_shares.push_back(top.asks.empty() ? 0 : top.asks[0].shares);
}

TopOfBookSeries::TopOfBookSeries(std::string symbol, std::uint64_t interval)
    : symbol_(std::move(symbol)), ticker_(interval) {}

void TopOfBookSeries::on_message(const MessageHeader& h) {
  ticker_.advance(h.timestamp, [this](std::uint64_t time) {
    // find() scans every symbol, so look the locate up once, not every tick.
    if (!locate_) {
      locate_ = locate();
    }
    columns_.add(time, locate_ ? top(*locate_, 1) : BookTop{});
  });
}

}  // namespace itchbook
