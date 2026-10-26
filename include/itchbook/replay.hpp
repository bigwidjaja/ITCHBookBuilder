#pragma once

#include "itchbook/book_v1.hpp"
#include "itchbook/format.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace itchbook {

// Regular market hours, in nanoseconds since midnight: 09:30:00 to 16:00:00.
inline constexpr std::uint64_t market_open = 34'200'000'000'000;
inline constexpr std::uint64_t market_close = 57'600'000'000'000;

// Fires at every whole multiple of an interval of exchange time. Call
// advance() with each message's timestamp before the message is applied: it
// calls on_tick(T) for each tick time T that the message has passed, so a tick
// sees the book after every message stamped T or earlier and none after.
// The first tick is the first whole interval at or after the first message,
// and times after the last message get no tick.
class Ticker {
 public:
  explicit Ticker(std::uint64_t interval) : interval_(interval) {}

  template <typename OnTick>
  void advance(std::uint64_t timestamp, OnTick&& on_tick) {
    if (!next_) {
      next_ = (timestamp + interval_ - 1) / interval_ * interval_;
    }
    while (timestamp > *next_) {
      on_tick(*next_);
      *next_ += interval_;
    }
  }

 private:
  std::uint64_t interval_;
  std::optional<std::uint64_t> next_;  // set by the first message
};

// The tools below work with any book that has BookV1's interface (BookV1,
// BookV2, ...), so each is a template that inherits from its book. The names
// without a T (CheckedBook, BookAt, ...) are the BookV1 versions.
//
// Inside these templates, calls to the book's own members are written
// this->top(...). Book is a template parameter, so the compiler cannot look
// names up in it until it knows which book it is; `this->` tells it to wait.

// A book plus checks from the outside. Counts every message, and after each
// book message checks that stock's book. A locked or crossed book during
// market hours counts as one "moment": normal continuous trading should never
// leave one, because an order that reaches the other side trades instead.
template <class Book>
class CheckedBookT : public Book {
 public:
  struct Problem {
    std::uint16_t locate;
    std::string_view symbol;
    std::uint64_t locked = 0;
    std::uint64_t crossed = 0;
    std::uint64_t first_time = 0;  // first locked or crossed moment
    std::uint64_t last_time = 0;   // last locked or crossed moment
  };

  void on_message(const MessageHeader&) override { ++messages_; }

  // Each book message is applied first, then the stock it touched is checked.
  void on_add(const AddOrder& m) override {
    Book::on_add(m);
    check(m.header);
  }
  void on_execute(const OrderExecuted& m) override {
    Book::on_execute(m);
    check(m.header);
  }
  void on_execute_with_price(const OrderExecutedWithPrice& m) override {
    Book::on_execute_with_price(m);
    check(m.header);
  }
  void on_cancel(const OrderCancel& m) override {
    Book::on_cancel(m);
    check(m.header);
  }
  void on_delete(const OrderDelete& m) override {
    Book::on_delete(m);
    check(m.header);
  }
  void on_replace(const OrderReplace& m) override {
    Book::on_replace(m);
    check(m.header);
  }

  std::uint64_t messages() const { return messages_; }
  std::uint64_t locked_moments() const { return locked_; }
  std::uint64_t crossed_moments() const { return crossed_; }

  // The stocks with the most locked plus crossed moments, most first.
  std::vector<Problem> worst(std::size_t n) const {
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

 private:
  void check(const MessageHeader& h) {
    if (h.timestamp < market_open || h.timestamp >= market_close) {
      return;
    }
    const BookState s = this->state(h.stock_locate);
    if (s == BookState::Normal) {
      return;
    }
    // try_emplace adds a Problem the first time this stock has one.
    auto [it, first] = problems_.try_emplace(h.stock_locate);
    Problem& p = it->second;
    if (first) {
      p.locate = h.stock_locate;
      p.symbol = this->symbols().symbol(h.stock_locate);
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

  std::uint64_t messages_ = 0;
  std::uint64_t locked_ = 0;
  std::uint64_t crossed_ = 0;
  std::unordered_map<std::uint16_t, Problem> problems_;  // stock locate -> its moments
};

// Replays up to a time and keeps one stock's top levels as they stood then:
// after every message stamped at or before `time`, and none after.
template <class Book>
class BookAtT : public Book {
 public:
  BookAtT(std::string symbol, std::uint64_t time, std::size_t depth)
      : symbol_(std::move(symbol)), time_(time), depth_(depth) {}

  // Runs before each message is applied, so the first message past `time`
  // sees the book exactly as it stood at `time`.
  void on_message(const MessageHeader& h) override {
    if (!snapshot_ && h.timestamp > time_) {
      snapshot_ = top_now();
    }
  }
  bool done() const override { return snapshot_.has_value(); }

  std::optional<std::uint16_t> locate() const { return this->symbols().find(symbol_); }

  // The snapshot, or the final book if the file ended before `time`.
  BookTop result() const { return snapshot_ ? *snapshot_ : top_now(); }

 private:
  BookTop top_now() const {
    const auto l = locate();
    return l ? this->top(*l, depth_) : BookTop{};
  }

  std::string symbol_;
  std::uint64_t time_;
  std::size_t depth_;
  std::optional<BookTop> snapshot_;
};

// Replays the whole file and writes a CSV snapshot of a few stocks' top
// levels at every tick of a Ticker.
//
// One row per time and stock:
//   time,symbol,bid_price_1,bid_shares_1,...,ask_price_1,ask_shares_1,...
// Missing levels are left empty.
template <class Book>
class GoldenWriterT : public CheckedBookT<Book> {
 public:
  static constexpr std::uint64_t default_interval = 60'000'000'000;  // 60 seconds

  GoldenWriterT(std::ostream& out, std::vector<std::string> symbols, std::size_t depth,
                std::uint64_t interval = default_interval)
      : out_(out), symbols_(std::move(symbols)), depth_(depth), ticker_(interval) {
    out_ << "time,symbol";
    for (const char* side : {"bid", "ask"}) {
      for (std::size_t i = 1; i <= depth_; ++i) {
        out_ << ',' << side << "_price_" << i << ',' << side << "_shares_" << i;
      }
    }
    out_ << '\n';
  }

  void on_message(const MessageHeader& h) override {
    ticker_.advance(h.timestamp, [this](std::uint64_t time) { write_snapshot(time); });
    CheckedBookT<Book>::on_message(h);
  }

  std::uint64_t rows() const { return rows_; }

 private:
  void write_snapshot(std::uint64_t time) {
    // HH:MM:SS.nnnnnnnnn; snapshots fall on whole seconds, so keep HH:MM:SS.
    const std::string clock = format_timestamp(time).substr(0, 8);
    for (const std::string& symbol : symbols_) {
      const auto locate = this->symbols().find(symbol);
      const BookTop t = locate ? this->top(*locate, depth_) : BookTop{};
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

  std::ostream& out_;
  std::vector<std::string> symbols_;
  std::size_t depth_;
  Ticker ticker_;
  std::uint64_t rows_ = 0;
};

// The best bid and ask at a series of times: entry i of every column belongs
// to time[i]. An empty side has price 0 and 0 shares.
struct TopOfBookColumns {
  std::vector<std::uint64_t> time;
  std::vector<std::uint32_t> bid_price;
  std::vector<std::uint64_t> bid_shares;
  std::vector<std::uint32_t> ask_price;
  std::vector<std::uint64_t> ask_shares;

  // Appends one row from the first level of each side of top.
  void add(std::uint64_t t, const BookTop& top);
};

// Replays the whole file and samples one stock's best bid and ask at every
// tick of a Ticker.
template <class Book>
class TopOfBookSeriesT : public Book {
 public:
  TopOfBookSeriesT(std::string symbol, std::uint64_t interval)
      : symbol_(std::move(symbol)), ticker_(interval) {}

  void on_message(const MessageHeader& h) override {
    ticker_.advance(h.timestamp, [this](std::uint64_t time) {
      // find() scans every symbol, so look the locate up once, not every tick.
      if (!locate_) {
        locate_ = locate();
      }
      columns_.add(time, locate_ ? this->top(*locate_, 1) : BookTop{});
    });
  }

  std::optional<std::uint16_t> locate() const { return this->symbols().find(symbol_); }
  const TopOfBookColumns& columns() const { return columns_; }

 private:
  std::string symbol_;
  std::optional<std::uint16_t> locate_;  // cached once an R message names the symbol
  Ticker ticker_;
  TopOfBookColumns columns_;
};

// Runs two books side by side and compares them after every book message: the
// touched stock's top `depth` levels on both sides, and the live order count.
// This checks every stock at every message, where the golden file checks 5
// stocks once a minute. A and B only need BookV1's interface.
template <class A, class B>
class DiffBooks : public Handler {
 public:
  explicit DiffBooks(std::size_t depth = 5) : depth_(depth) {}

  void on_stock_directory(const StockDirectory& m) override {
    a_.on_stock_directory(m);
    b_.on_stock_directory(m);
  }
  void on_add(const AddOrder& m) override { both(m, &A::on_add, &B::on_add); }
  void on_execute(const OrderExecuted& m) override { both(m, &A::on_execute, &B::on_execute); }
  void on_execute_with_price(const OrderExecutedWithPrice& m) override {
    both(m, &A::on_execute_with_price, &B::on_execute_with_price);
  }
  void on_cancel(const OrderCancel& m) override { both(m, &A::on_cancel, &B::on_cancel); }
  void on_delete(const OrderDelete& m) override { both(m, &A::on_delete, &B::on_delete); }
  void on_replace(const OrderReplace& m) override { both(m, &A::on_replace, &B::on_replace); }

  std::uint64_t compared() const { return compared_; }
  std::uint64_t mismatches() const { return mismatches_; }
  // The first message after which the books differed, if any.
  const std::optional<MessageHeader>& first_mismatch() const { return first_mismatch_; }
  const A& a() const { return a_; }
  const B& b() const { return b_; }

 private:
  // Applies m to both books, then compares the stock it touched.
  template <class Message, class FnA, class FnB>
  void both(const Message& m, FnA on_a, FnB on_b) {
    (a_.*on_a)(m);
    (b_.*on_b)(m);
    ++compared_;
    const std::uint16_t locate = m.header.stock_locate;
    const BookTop ta = a_.top(locate, depth_);
    const BookTop tb = b_.top(locate, depth_);
    if (ta.bids != tb.bids || ta.asks != tb.asks || a_.order_count() != b_.order_count()) {
      ++mismatches_;
      if (!first_mismatch_) {
        first_mismatch_ = m.header;
      }
    }
  }

  A a_;
  B b_;
  std::size_t depth_;
  std::uint64_t compared_ = 0;
  std::uint64_t mismatches_ = 0;
  std::optional<MessageHeader> first_mismatch_;
};

// The BookV1 versions, under the names the rest of the code has always used.
using CheckedBook = CheckedBookT<BookV1>;
using BookAt = BookAtT<BookV1>;
using GoldenWriter = GoldenWriterT<BookV1>;
using TopOfBookSeries = TopOfBookSeriesT<BookV1>;

}  // namespace itchbook
