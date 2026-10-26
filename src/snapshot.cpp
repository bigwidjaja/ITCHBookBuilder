#include "itchbook/snapshot.hpp"

#include "itchbook/core.hpp"
#include "itchbook/messages.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>

namespace itchbook {

namespace {

// Rough heap bytes held by one snapshot's book. libc++ allocates each
// unordered_map entry as a 48-byte node plus about one 8-byte bucket pointer,
// and each std::map entry as a 48-byte node. Small fixed costs are left out.
std::size_t snapshot_bytes(const BookV1& book) {
  return book.order_count() * (48 + 8) + book.level_count() * 48;
}

// The messages a per-symbol book applies. R is left out so that each copied
// book's symbol directory stays empty.
bool is_book_type(char type) {
  switch (type) {
    case 'A':
    case 'F':
    case 'E':
    case 'C':
    case 'X':
    case 'D':
    case 'U':
      return true;
    default:
      return false;
  }
}

// The header of a message that parse_message would accept, or nullopt.
std::optional<MessageHeader> valid_header(const std::uint8_t* p, std::size_t size) {
  if (size == 0 || size != expected_lengths[p[0]]) {
    return std::nullopt;
  }
  return decode_header(p);
}

}  // namespace

SnapshotIndex::SnapshotIndex(std::filesystem::path path, const std::vector<std::string>& symbols,
                             std::uint64_t interval)
    : file_(path) {
  const auto start = std::chrono::steady_clock::now();
  stats_.interval = interval;

  std::vector<Symbol> chosen(symbols.size());
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    chosen[i].name = symbols[i];
  }
  std::vector<BookV1> live(symbols.size());
  std::vector<bool> named(symbols.size(), false);
  std::vector<int> slot_of(65536, -1);  // stock locate -> index into chosen, or -1
  std::optional<Ticker> ticker;
  if (interval != 0) {
    ticker.emplace(interval);
  }

  MessageReader reader(file_.bytes());
  while (reader.next()) {
    const auto h = valid_header(reader.data(), reader.size());
    if (!h) {
      continue;
    }
    ++stats_.messages;
    ++stats_.by_type[static_cast<unsigned char>(h->type)];

    // Snapshots are taken before this message is applied, and resume at it.
    if (ticker) {
      ticker->advance(h->timestamp, [&](std::uint64_t time) {
        for (std::size_t i = 0; i < chosen.size(); ++i) {
          if (named[i]) {
            chosen[i].snapshots.push_back({time, reader.offset(), live[i]});
            ++stats_.snapshots;
            stats_.snapshot_bytes += snapshot_bytes(live[i]);
          }
        }
      });
    }

    if (h->type == 'R') {
      const StockDirectory r = decode_stock_directory(reader.data());
      for (std::size_t i = 0; i < chosen.size(); ++i) {
        if (!named[i] && r.stock == chosen[i].name) {
          named[i] = true;
          chosen[i].locate = h->stock_locate;
          slot_of[h->stock_locate] = static_cast<int>(i);
        }
      }
      continue;
    }
    const int slot = slot_of[h->stock_locate];
    if (slot >= 0 && is_book_type(h->type)) {
      parse_message(reader.data(), reader.size(), live[static_cast<std::size_t>(slot)]);
    }
  }

  for (std::size_t i = 0; i < chosen.size(); ++i) {
    if (named[i]) {
      symbols_.push_back(std::move(chosen[i]));
    }
  }
  stats_.build_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::vector<std::string> SnapshotIndex::symbols() const {
  std::vector<std::string> names;
  for (const Symbol& s : symbols_) {
    names.push_back(s.name);
  }
  return names;
}

const SnapshotIndex::Symbol* SnapshotIndex::find(std::string_view name) const {
  for (const Symbol& s : symbols_) {
    if (s.name == name) {
      return &s;
    }
  }
  return nullptr;
}

template <typename Sample>
void SnapshotIndex::replay(const Symbol& s, const std::vector<std::uint64_t>& times,
                           Sample&& sample) const {
  // upper_bound finds the first snapshot after times.front(); the one before
  // it is the latest at or before. None before it means start from scratch.
  const auto after = std::upper_bound(
      s.snapshots.begin(), s.snapshots.end(), times.front(),
      [](std::uint64_t t, const Snapshot& snap) { return t < snap.time; });
  BookV1 book;
  std::uint64_t offset = 0;
  if (after != s.snapshots.begin()) {
    const Snapshot& snap = *std::prev(after);
    book = snap.book;  // a copy: the stored snapshot must stay as it was
    offset = snap.offset;
  }

  std::size_t next = 0;
  MessageReader reader(file_.bytes(), offset);
  while (next < times.size() && reader.next()) {
    const auto h = valid_header(reader.data(), reader.size());
    if (!h) {
      continue;
    }
    // This message is past each of these times and not applied yet, so the
    // book is exactly as it stood at them.
    while (next < times.size() && h->timestamp > times[next]) {
      sample(times[next], book);
      ++next;
    }
    if (h->stock_locate == s.locate && is_book_type(h->type)) {
      parse_message(reader.data(), reader.size(), book);
    }
  }
  for (; next < times.size(); ++next) {
    sample(times[next], book);  // the file ended before these times
  }
}

std::optional<BookTop> SnapshotIndex::book(std::string_view symbol, std::uint64_t at,
                                           std::size_t depth) const {
  const Symbol* s = find(symbol);
  if (s == nullptr) {
    return std::nullopt;
  }
  BookTop out;
  replay(*s, {at}, [&](std::uint64_t, const BookV1& b) { out = b.top(s->locate, depth); });
  return out;
}

std::optional<TopOfBookColumns> SnapshotIndex::bbo(std::string_view symbol, std::uint64_t start,
                                                   std::uint64_t end,
                                                   std::uint64_t interval) const {
  const Symbol* s = find(symbol);
  if (s == nullptr) {
    return std::nullopt;
  }
  TopOfBookColumns columns;
  if (interval == 0 || start > end) {
    return columns;
  }
  std::vector<std::uint64_t> times;
  for (std::uint64_t t = start; t <= end; t += interval) {
    times.push_back(t);
  }
  replay(*s, times,
         [&](std::uint64_t t, const BookV1& b) { columns.add(t, b.top(s->locate, 1)); });
  return columns;
}

}  // namespace itchbook
