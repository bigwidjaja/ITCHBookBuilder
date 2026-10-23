#pragma once

#include "itchbook/book_v1.hpp"
#include "itchbook/replay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace itchbook {

// Answers "what did this stock's book look like at time t?" without replaying
// the day from the start.
//
// Building it replays the file once. Each chosen symbol gets its own BookV1,
// fed only that symbol's messages: every ITCH message carries its stock
// locate, so the stream splits cleanly by stock. Every `interval` of exchange
// time it saves a copy of each of those books, live orders included, and the
// file offset of the next message. A query copies the latest snapshot at or
// before t and replays from that offset, applying only that symbol's messages.
//
// interval 0 means no snapshots: every query replays from the start of the file.
// Queries are const and open their own reader, so threads can run them at once.
class SnapshotIndex {
 public:
  struct Stats {
    std::uint64_t messages = 0;
    std::array<std::uint64_t, 256> by_type{};
    std::uint64_t interval = 0;
    std::size_t snapshots = 0;       // across all symbols
    std::size_t snapshot_bytes = 0;  // estimate, see snapshot_bytes() in snapshot.cpp
    double build_seconds = 0;
  };

  SnapshotIndex(std::filesystem::path path, const std::vector<std::string>& symbols,
                std::uint64_t interval);

  // The chosen symbols that an R message named, in the order they were given.
  std::vector<std::string> symbols() const;

  // symbol's book after every message stamped at or before `at`.
  // nullopt if symbol is not indexed.
  std::optional<BookTop> book(std::string_view symbol, std::uint64_t at, std::size_t depth) const;

  // symbol's best bid and ask at start, start + interval, ... up to end.
  // nullopt if symbol is not indexed.
  std::optional<TopOfBookColumns> bbo(std::string_view symbol, std::uint64_t start,
                                      std::uint64_t end, std::uint64_t interval) const;

  const Stats& stats() const { return stats_; }

 private:
  struct Snapshot {
    std::uint64_t time;    // the book after every message stamped at or before this
    std::uint64_t offset;  // where the first message after `time` starts in the file
    BookV1 book;
  };

  struct Symbol {
    std::string name;
    std::uint16_t locate = 0;
    std::vector<Snapshot> snapshots;  // in time order
  };

  const Symbol* find(std::string_view name) const;

  // Replays s from its latest snapshot at or before times.front(), and calls
  // sample(time, book) for each of `times` (ascending) with the book as it
  // stood at that time.
  template <typename Sample>
  void replay(const Symbol& s, const std::vector<std::uint64_t>& times, Sample&& sample) const;

  std::filesystem::path path_;
  std::vector<Symbol> symbols_;
  Stats stats_;
};

}  // namespace itchbook
