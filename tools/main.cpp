#include "itchbook/core.hpp"
#include "itchbook/dump.hpp"
#include "itchbook/format.hpp"
#include "itchbook/mapped_file.hpp"
#include "itchbook/book_v2.hpp"
#include "itchbook/replay.hpp"
#include "itchbook/stats.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

int print_file_size(const std::filesystem::path& path) {
  std::cout << itchbook::file_size(path) << '\n';
  return 0;
}

int count_file(const std::filesystem::path& path) {
  const auto start = std::chrono::steady_clock::now();
  const itchbook::MessageCounts result = itchbook::count_messages(path);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  const double msg_per_s = seconds > 0 ? static_cast<double>(result.messages) / seconds : 0;

  std::cout << "messages: " << result.messages << '\n';
  std::cout << "mismatches: " << result.mismatches << '\n';
  std::cout << "truncated: " << (result.truncated ? "yes" : "no") << '\n';
  std::cout << std::fixed << std::setprecision(6);
  std::cout << "seconds: " << seconds << '\n';
  std::cout << "msg_per_s: " << msg_per_s << '\n';
  for (int type = 0; type < 256; ++type) {
    if (result.by_type[static_cast<std::size_t>(type)] == 0) {
      continue;
    }
    const auto count = result.by_type[static_cast<std::size_t>(type)];
    if (type >= 32 && type < 127) {
      std::cout << static_cast<char>(type);
    } else {
      std::cout << "0x" << std::hex << type << std::dec;
    }
    std::cout << ' ' << count << '\n';
  }
  return 0;
}

constexpr std::string_view book_names = "v1, v2, v2-std, v2-direct, v2-bsearch";

int usage() {
  std::cerr << "usage: itchbook PATH\n"
            << "       itchbook count PATH\n"
            << "       itchbook dump PATH [--type T] [--limit N]\n"
            << "       itchbook stats PATH\n"
            << "       itchbook parse PATH\n"
            << "       itchbook book PATH --symbol S --at HH:MM:SS [--depth N] [--book NAME]\n"
            << "       itchbook book PATH [--golden OUT.csv] [--book NAME] [--dispatch virtual|template]\n"
            << "       itchbook diff PATH [--book NAME]     (compares BookV1 with NAME, default v2)\n"
            << "  --book: " << book_names << " (default v2)\n";
  return 1;
}

// Parses value as a whole number into out. Returns false if it is not one.
bool parse_count(std::string_view value, std::uint64_t& out) {
  const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
  return ec == std::errc{} && end == value.data() + value.size();
}

// Decodes every message and hands it to a handler that only counts: the cost
// of reading and parsing on its own, without a book. Compare with `book` to
// see the book's share.
int parse_only(const std::filesystem::path& path) {
  struct Counter : itchbook::Handler {
    std::uint64_t messages = 0;
    void on_message(const itchbook::MessageHeader&) override { ++messages; }
  } handler;

  const auto start = std::chrono::steady_clock::now();
  itchbook::parse_file(path, handler);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout << "messages: " << handler.messages << '\n';
  std::cout << std::format("seconds: {:.3f}\n", seconds);
  std::cout << std::format("msg_per_s: {:.0f}\n",
                           seconds > 0 ? static_cast<double>(handler.messages) / seconds : 0);
  return 0;
}

int stats_file(const std::filesystem::path& path) {
  itchbook::StatsHandler handler;
  itchbook::parse_file(path, handler);
  handler.print(std::cout);
  return 0;
}

// The types parse_message decodes. Any other type would print nothing.
constexpr std::string_view decoded_types = "SRAFECXDU";

// argv[2] is the path. The rest are flag/value pairs: --type T and --limit N.
int dump_file(int argc, char** argv) {
  char type = 0;
  std::uint64_t limit = 0;
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 >= argc) {
      return usage();
    }
    const std::string_view flag = argv[i];
    const std::string_view value = argv[i + 1];
    if (flag == "--type") {
      if (value.size() != 1 || decoded_types.find(value[0]) == std::string_view::npos) {
        std::cerr << "--type must be one of: " << decoded_types << '\n';
        return 1;
      }
      type = value[0];
    } else if (flag == "--limit") {
      if (!parse_count(value, limit)) {
        std::cerr << "--limit must be a whole number\n";
        return 1;
      }
    } else {
      return usage();
    }
  }

  itchbook::DumpHandler handler(std::cout, type, limit);
  itchbook::parse_file(argv[2], handler);
  return 0;
}

// The golden file's 5 liquid symbols: the three biggest index ETFs and two of
// the most traded stocks of the day (see notes/data-shape.md).
const std::vector<std::string> golden_symbols{"QQQ", "SPY", "IWM", "AAPL", "MSFT"};
constexpr std::size_t golden_depth = 5;

// Prints both sides next to each other, best level on the first row.
void print_top(const itchbook::BookTop& t) {
  std::cout << std::format("{:>10} {:>12} | {:<12} {:<10}\n", "shares", "bid", "ask", "shares");
  const std::size_t rows = std::max(t.bids.size(), t.asks.size());
  for (std::size_t i = 0; i < rows; ++i) {
    std::string bid = std::format("{:>10} {:>12}", "", "");
    std::string ask;
    if (i < t.bids.size()) {
      bid = std::format("{:>10} {:>12}", t.bids[i].shares, itchbook::format_price(t.bids[i].price));
    }
    if (i < t.asks.size()) {
      ask = std::format("{:<12} {:<10}", itchbook::format_price(t.asks[i].price), t.asks[i].shares);
    }
    std::cout << bid << " | " << ask << '\n';
  }
}

// The books --book can name; with_book below turns a name into a type.
// f is a template lambda, `[&]<class Book>() {...}`, and with_book calls it
// with the named book type, or returns nullopt if no book has that name.

template <class F>
std::optional<int> with_book(std::string_view name, F&& f) {
  if (name == "v1") {
    return f.template operator()<itchbook::BookV1>();
  }
  if (name == "v2") {
    return f.template operator()<itchbook::BookV2>();
  }
  if (name == "v2-std") {
    return f.template operator()<itchbook::BookV2Std>();
  }
  if (name == "v2-direct") {
    return f.template operator()<itchbook::BookV2Direct>();
  }
  if (name == "v2-bsearch") {
    return f.template operator()<itchbook::BookV2BinarySearch>();
  }
  return std::nullopt;
}

// Sizes a book's order table for a whole day, if the book supports it.
// `requires { ... }` asks the compiler whether that expression would compile
// for this Book; BookV1 has no reserve_orders on purpose, so for it the call
// is left out entirely.
template <class Book>
void reserve_for_day(Book& book) {
  if constexpr (requires { book.reserve_orders(std::size_t{}); }) {
    book.reserve_orders(itchbook::day_peak_live_orders);
  }
}

// Runs every message of the file through h. With virtual_calls, through
// parse_file and a Handler& (a virtual call per message, Step 2's way);
// otherwise through parse<H> on h's real type, which the compiler can call
// directly and inline when H is final (Step 12). Same results either way.
template <class H>
void run(const std::filesystem::path& path, H& h, bool virtual_calls) {
  if (virtual_calls) {
    itchbook::parse_file(path, static_cast<itchbook::Handler&>(h));
  } else {
    const itchbook::MappedFile file(path);
    itchbook::parse(file.bytes(), h);
  }
}

template <class Book>
int book_at(const std::filesystem::path& path, const std::string& symbol, std::uint64_t time,
            std::size_t depth, bool virtual_calls) {
  itchbook::BookAtT<Book> book(symbol, time, depth);
  reserve_for_day(book);
  run(path, book, virtual_calls);
  const auto locate = book.locate();
  if (!locate) {
    std::cerr << "no R message names " << symbol << '\n';
    return 1;
  }
  std::cout << symbol << " (locate " << *locate << ") at " << itchbook::format_timestamp(time)
            << '\n';
  print_top(book.result());
  return 0;
}

// Replays the whole file through book (a CheckedReplayT or GoldenWriterT) and
// prints the report.
template <class Checked>
void replay_and_report(const std::filesystem::path& path, Checked& book, bool virtual_calls) {
  reserve_for_day(book);
  const auto start = std::chrono::steady_clock::now();
  run(path, book, virtual_calls);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  const double msg_per_s = seconds > 0 ? static_cast<double>(book.messages()) / seconds : 0;

  std::cout << "messages: " << book.messages() << '\n';
  std::cout << std::format("seconds: {:.3f}\n", seconds);
  std::cout << std::format("msg_per_s: {:.0f}\n", msg_per_s);
  std::cout << "errors: " << book.errors() << '\n';
  std::cout << "live orders at end: " << book.order_count() << '\n';
  std::cout << "peak live orders: " << book.peak_order_count() << '\n';
  std::cout << "market hours locked moments: " << book.locked_moments() << '\n';
  std::cout << "market hours crossed moments: " << book.crossed_moments() << '\n';
  const auto worst = book.worst(10);
  if (!worst.empty()) {
    std::cout << "\nmost locked or crossed:\n";
    std::cout << std::format("  {:<8} {:>8} {:>8}  {:<18}  {:<18}\n", "symbol", "locked", "crossed",
                             "first", "last");
    for (const auto& p : worst) {
      std::cout << std::format("  {:<8} {:>8} {:>8}  {:<18}  {:<18}\n", p.symbol, p.locked,
                               p.crossed, itchbook::format_timestamp(p.first_time),
                               itchbook::format_timestamp(p.last_time));
    }
  }
}

// Replays the whole file and reports on it. With a golden path, also writes
// the golden CSV. Each branch uses its own final type, so the template path
// can call the book directly.
template <class Book>
int book_replay(const std::filesystem::path& path, const std::string& golden_path,
                bool virtual_calls) {
  if (golden_path.empty()) {
    auto book = std::make_unique<itchbook::CheckedReplayT<Book>>();
    replay_and_report(path, *book, virtual_calls);
    return 0;
  }
  std::ofstream golden(golden_path);
  if (!golden) {
    std::cerr << "cannot write " << golden_path << '\n';
    return 1;
  }
  auto writer = std::make_unique<itchbook::GoldenWriterT<Book>>(golden, golden_symbols, golden_depth);
  replay_and_report(path, *writer, virtual_calls);
  std::cout << "\ngolden: " << writer->rows() << " rows in " << golden_path << '\n';
  return 0;
}

// Replays the file into BookV1 and Book side by side, comparing the touched
// stock's top 5 levels and the live order count after every book message.
template <class Book>
int diff_file(const std::filesystem::path& path) {
  auto diff = std::make_unique<itchbook::DiffBooks<itchbook::BookV1, Book>>();
  const auto start = std::chrono::steady_clock::now();
  run(path, *diff, false);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::cout << "book messages compared: " << diff->compared() << '\n';
  std::cout << "mismatches: " << diff->mismatches() << '\n';
  if (const auto& m = diff->first_mismatch()) {
    std::cout << "first mismatch: " << m->type << " message for locate " << m->stock_locate
              << " at " << itchbook::format_timestamp(m->timestamp) << '\n';
  }
  std::cout << "errors: v1 " << diff->a().errors() << ", other " << diff->b().errors() << '\n';
  std::cout << "live orders at end: v1 " << diff->a().order_count() << ", other "
            << diff->b().order_count() << '\n';
  std::cout << std::format("seconds: {:.3f}\n", seconds);
  return diff->mismatches() == 0 ? 0 : 1;
}

int diff_command(int argc, char** argv) {
  std::string_view book = "v2";
  if (argc == 5 && std::string_view(argv[3]) == "--book") {
    book = argv[4];
  } else if (argc != 3) {
    return usage();
  }
  const auto result = with_book(book, [&]<class Book>() { return diff_file<Book>(argv[2]); });
  if (!result) {
    std::cerr << "--book must be one of: " << book_names << '\n';
    return 1;
  }
  return *result;
}

// argv[2] is the path. The rest are flag/value pairs.
int book_file(int argc, char** argv) {
  std::string symbol;
  std::optional<std::uint64_t> time;
  std::uint64_t depth = 5;
  std::string golden_path;
  std::string_view book = "v2";
  bool virtual_calls = false;
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 >= argc) {
      return usage();
    }
    const std::string_view flag = argv[i];
    const std::string_view value = argv[i + 1];
    if (flag == "--book") {
      book = value;
    } else if (flag == "--dispatch") {
      if (value != "virtual" && value != "template") {
        std::cerr << "--dispatch must be virtual or template\n";
        return 1;
      }
      virtual_calls = value == "virtual";
    } else if (flag == "--symbol") {
      symbol = value;
    } else if (flag == "--at") {
      time = itchbook::parse_timestamp(value);
      if (!time) {
        std::cerr << "--at must look like HH:MM:SS or HH:MM:SS.nnnnnnnnn\n";
        return 1;
      }
    } else if (flag == "--depth") {
      if (!parse_count(value, depth) || depth == 0) {
        std::cerr << "--depth must be a whole number of at least 1\n";
        return 1;
      }
    } else if (flag == "--golden") {
      golden_path = value;
    } else {
      return usage();
    }
  }

  if (!symbol.empty() != time.has_value() || (time && !golden_path.empty())) {
    return usage();  // --symbol and --at go together, and not with --golden
  }
  const auto result = with_book(book, [&]<class Book>() {
    return time ? book_at<Book>(argv[2], symbol, *time, depth, virtual_calls)
                : book_replay<Book>(argv[2], golden_path, virtual_calls);
  });
  if (!result) {
    std::cerr << "--book must be one of: " << book_names << '\n';
    return 1;
  }
  return *result;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc >= 3 && std::strcmp(argv[1], "dump") == 0) {
      return dump_file(argc, argv);
    }
    if (argc >= 3 && std::strcmp(argv[1], "book") == 0) {
      return book_file(argc, argv);
    }
    if (argc >= 3 && std::strcmp(argv[1], "diff") == 0) {
      return diff_command(argc, argv);
    }
    if (argc == 3 && std::strcmp(argv[1], "stats") == 0) {
      return stats_file(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "parse") == 0) {
      return parse_only(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "count") == 0) {
      return count_file(argv[2]);
    }
    if (argc == 2) {
      return print_file_size(argv[1]);
    }
  } catch (const std::filesystem::filesystem_error& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }

  return usage();
}
