#include "itchbook/core.hpp"
#include "itchbook/dump.hpp"
#include "itchbook/format.hpp"
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

int usage() {
  std::cerr << "usage: itchbook PATH\n"
            << "       itchbook count PATH\n"
            << "       itchbook dump PATH [--type T] [--limit N]\n"
            << "       itchbook stats PATH\n"
            << "       itchbook book PATH --symbol S --at HH:MM:SS [--depth N]\n"
            << "       itchbook book PATH [--golden OUT.csv]\n";
  return 1;
}

// Parses value as a whole number into out. Returns false if it is not one.
bool parse_count(std::string_view value, std::uint64_t& out) {
  const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
  return ec == std::errc{} && end == value.data() + value.size();
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

int book_at(const std::filesystem::path& path, const std::string& symbol, std::uint64_t time,
            std::size_t depth) {
  itchbook::BookAt book(symbol, time, depth);
  itchbook::parse_file(path, book);
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

// Replays the whole file and reports on it. With a golden path, also writes
// the golden CSV.
int book_replay(const std::filesystem::path& path, const std::string& golden_path) {
  std::ofstream golden;
  std::unique_ptr<itchbook::CheckedBook> book;
  const itchbook::GoldenWriter* writer = nullptr;
  if (golden_path.empty()) {
    book = std::make_unique<itchbook::CheckedBook>();
  } else {
    golden.open(golden_path);
    if (!golden) {
      std::cerr << "cannot write " << golden_path << '\n';
      return 1;
    }
    auto w = std::make_unique<itchbook::GoldenWriter>(golden, golden_symbols, golden_depth);
    writer = w.get();
    book = std::move(w);
  }

  const auto start = std::chrono::steady_clock::now();
  itchbook::parse_file(path, *book);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  const double msg_per_s = seconds > 0 ? static_cast<double>(book->messages()) / seconds : 0;

  std::cout << "messages: " << book->messages() << '\n';
  std::cout << std::format("seconds: {:.3f}\n", seconds);
  std::cout << std::format("msg_per_s: {:.0f}\n", msg_per_s);
  std::cout << "errors: " << book->errors() << '\n';
  std::cout << "live orders at end: " << book->order_count() << '\n';
  std::cout << "peak live orders: " << book->peak_order_count() << '\n';
  std::cout << "market hours locked moments: " << book->locked_moments() << '\n';
  std::cout << "market hours crossed moments: " << book->crossed_moments() << '\n';
  const auto worst = book->worst(10);
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
  if (writer != nullptr) {
    std::cout << "\ngolden: " << writer->rows() << " rows in " << golden_path << '\n';
  }
  return 0;
}

// argv[2] is the path. The rest are flag/value pairs.
int book_file(int argc, char** argv) {
  std::string symbol;
  std::optional<std::uint64_t> time;
  std::uint64_t depth = 5;
  std::string golden_path;
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 >= argc) {
      return usage();
    }
    const std::string_view flag = argv[i];
    const std::string_view value = argv[i + 1];
    if (flag == "--symbol") {
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

  if (symbol.empty() && !time) {
    return book_replay(argv[2], golden_path);
  }
  if (symbol.empty() || !time || !golden_path.empty()) {
    return usage();  // --symbol and --at go together, and not with --golden
  }
  return book_at(argv[2], symbol, *time, depth);
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
    if (argc == 3 && std::strcmp(argv[1], "stats") == 0) {
      return stats_file(argv[2]);
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
