#pragma once

#include "itchbook/book_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
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

// BookV1 plus checks from the outside. Counts every message, and after each
// book message checks that stock's book. A locked or crossed book during
// market hours counts as one "moment": normal continuous trading should never
// leave one, because an order that reaches the other side trades instead.
class CheckedBook : public BookV1 {
 public:
  struct Problem {
    std::uint16_t locate;
    std::string_view symbol;
    std::uint64_t locked = 0;
    std::uint64_t crossed = 0;
    std::uint64_t first_time = 0;  // first locked or crossed moment
    std::uint64_t last_time = 0;   // last locked or crossed moment
  };

  void on_message(const MessageHeader& h) override;
  void on_add(const AddOrder& m) override;
  void on_execute(const OrderExecuted& m) override;
  void on_execute_with_price(const OrderExecutedWithPrice& m) override;
  void on_cancel(const OrderCancel& m) override;
  void on_delete(const OrderDelete& m) override;
  void on_replace(const OrderReplace& m) override;

  std::uint64_t messages() const { return messages_; }
  std::uint64_t locked_moments() const { return locked_; }
  std::uint64_t crossed_moments() const { return crossed_; }

  // The stocks with the most locked plus crossed moments, most first.
  std::vector<Problem> worst(std::size_t n) const;

 private:
  void check(const MessageHeader& h);

  std::uint64_t messages_ = 0;
  std::uint64_t locked_ = 0;
  std::uint64_t crossed_ = 0;
  std::unordered_map<std::uint16_t, Problem> problems_;  // stock locate -> its moments
};

// Replays up to a time and keeps one stock's top levels as they stood then:
// after every message stamped at or before `time`, and none after.
class BookAt : public BookV1 {
 public:
  BookAt(std::string symbol, std::uint64_t time, std::size_t depth);

  // Runs before each message is applied, so the first message past `time`
  // sees the book exactly as it stood at `time`.
  void on_message(const MessageHeader& h) override;
  bool done() const override { return snapshot_.has_value(); }

  std::optional<std::uint16_t> locate() const { return symbols().find(symbol_); }

  // The snapshot, or the final book if the file ended before `time`.
  BookTop result() const;

 private:
  BookTop top_now() const;

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
class GoldenWriter : public CheckedBook {
 public:
  static constexpr std::uint64_t default_interval = 60'000'000'000;  // 60 seconds

  GoldenWriter(std::ostream& out, std::vector<std::string> symbols, std::size_t depth,
               std::uint64_t interval = default_interval);

  void on_message(const MessageHeader& h) override;

  std::uint64_t rows() const { return rows_; }

 private:
  void write_snapshot(std::uint64_t time);

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
class TopOfBookSeries : public BookV1 {
 public:
  TopOfBookSeries(std::string symbol, std::uint64_t interval);

  void on_message(const MessageHeader& h) override;

  std::optional<std::uint16_t> locate() const { return symbols().find(symbol_); }
  const TopOfBookColumns& columns() const { return columns_; }

 private:
  std::string symbol_;
  std::optional<std::uint16_t> locate_;  // cached once an R message names the symbol
  Ticker ticker_;
  TopOfBookColumns columns_;
};

}  // namespace itchbook
