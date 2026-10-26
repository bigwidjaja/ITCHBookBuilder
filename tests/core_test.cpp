#include "itchbook/book_v1.hpp"
#include "itchbook/book_v2.hpp"
#include "itchbook/core.hpp"
#include "itchbook/dump.hpp"
#include "itchbook/flat_map.hpp"
#include "itchbook/format.hpp"
#include "itchbook/mapped_file.hpp"
#include "itchbook/messages.hpp"
#include "itchbook/replay.hpp"
#include "itchbook/snapshot.hpp"
#include "itchbook/stats.hpp"
#include "itchbook/symbols.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <sys/resource.h>  // getrlimit

TEST(Core, FileSize) {
  const auto path = std::filesystem::temp_directory_path() / "itchbook-file-size-test";
  {
    std::ofstream out(path, std::ios::binary);
    out << "abcd";
  }

  EXPECT_EQ(itchbook::file_size(path), std::uintmax_t{4});
  std::filesystem::remove(path);
}

TEST(Core, FileSizeMissingThrows) {
  EXPECT_THROW(itchbook::file_size("itchbook-missing-file"), std::filesystem::filesystem_error);
}

TEST(Core, OpenBinary) {
  const auto path = std::filesystem::temp_directory_path() / "itchbook-open-binary-test";
  {
    std::ofstream out(path, std::ios::binary);
    out << "abcd";
  }

  std::ifstream in = itchbook::open_binary(path);
  EXPECT_TRUE(static_cast<bool>(in));
  std::filesystem::remove(path);
}

TEST(Core, OpenBinaryMissingThrows) {
  EXPECT_THROW(itchbook::open_binary("itchbook-missing-file"), std::filesystem::filesystem_error);
}

namespace {

std::filesystem::path write_bytes(const char* name, const std::vector<std::uint8_t>& bytes) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return path;
}

}  // namespace

TEST(Core, ReadMessages) {
  const auto path = write_bytes("itchbook-read-messages", {0x00, 0x03, 'A', 'B', 'C', 0x00, 0x01, 'Z'});
  const auto messages = itchbook::read_messages(path);
  std::filesystem::remove(path);

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0], std::vector<std::uint8_t>({'A', 'B', 'C'}));
  EXPECT_EQ(messages[1], std::vector<std::uint8_t>({'Z'}));
}

TEST(Core, ReadMessagesUsesHighLengthByte) {
  std::vector<std::uint8_t> bytes{0x01, 0x00};
  bytes.insert(bytes.end(), 256, 0xAB);
  const auto path = write_bytes("itchbook-read-messages-high-byte", bytes);

  const auto messages = itchbook::read_messages(path);
  std::filesystem::remove(path);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].size(), 256u);
}

TEST(Core, CountMessagesByType) {
  const auto path = write_bytes(
      "itchbook-count-messages",
      {0x00, 0x01, 'A', 0x00, 0x01, 'A', 0x00, 0x03, 'D', 'x', 'y'});
  const auto counts = itchbook::count_messages(path);
  std::filesystem::remove(path);

  EXPECT_EQ(counts.by_type['A'], 2u);
  EXPECT_EQ(counts.by_type['D'], 1u);
  EXPECT_EQ(counts.by_type['Z'], 0u);
  EXPECT_EQ(counts.messages, 3u);
  EXPECT_EQ(counts.mismatches, 3u);
  EXPECT_FALSE(counts.truncated);
}

TEST(Core, CountMessagesSkipsCutOff) {
  const auto path = write_bytes("itchbook-count-cutoff", {0x00, 0x01, 'A', 0x00, 0x04, 'D', 'x'});
  const auto counts = itchbook::count_messages(path);
  std::filesystem::remove(path);

  EXPECT_EQ(counts.by_type['A'], 1u);
  EXPECT_EQ(counts.by_type['D'], 0u);
  EXPECT_TRUE(counts.truncated);
  EXPECT_EQ(counts.messages, 1u);
}

TEST(Core, ExpectedLengthsDistinguishHAndh) {
  EXPECT_EQ(itchbook::expected_lengths['H'], 25u);
  EXPECT_EQ(itchbook::expected_lengths['h'], 21u);
  EXPECT_EQ(itchbook::expected_lengths['A'], 36u);
  EXPECT_EQ(itchbook::expected_lengths['Z'], 0u);
}

TEST(Core, CountMessagesMatchingLength) {
  std::vector<std::uint8_t> bytes{0x00, 12, 'S'};
  bytes.insert(bytes.end(), 11, 0);
  const auto path = write_bytes("itchbook-count-match", bytes);
  const auto counts = itchbook::count_messages(path);
  std::filesystem::remove(path);

  EXPECT_EQ(counts.messages, 1u);
  EXPECT_EQ(counts.mismatches, 0u);
  EXPECT_FALSE(counts.truncated);
  EXPECT_EQ(counts.by_type['S'], 1u);
}

TEST(Core, CountMessagesReportsTruncatedLength) {
  const auto path = write_bytes("itchbook-count-short-length", {0x00});
  const auto counts = itchbook::count_messages(path);
  std::filesystem::remove(path);

  EXPECT_TRUE(counts.truncated);
  EXPECT_EQ(counts.messages, 0u);
}

TEST(LoadBe, Be16) {
  const std::array<std::uint8_t, 2> one{0x00, 0x01};
  const std::array<std::uint8_t, 2> two_fifty_six{0x01, 0x00};
  const std::array<std::uint8_t, 2> high{0xAB, 0xCD};

  EXPECT_EQ(itchbook::load_be16(one.data()), 1u);
  EXPECT_EQ(itchbook::load_be16(two_fifty_six.data()), 256u);
  EXPECT_EQ(itchbook::load_be16(high.data()), 0xABCDu);
}

TEST(LoadBe, Be32) {
  const std::array<std::uint8_t, 4> ordered{0x01, 0x02, 0x03, 0x04};
  const std::array<std::uint8_t, 4> max{0xFF, 0xFF, 0xFF, 0xFF};
  // $123.4500 as an ITCH price: 1234500 = 0x0012D644.
  const std::array<std::uint8_t, 4> price{0x00, 0x12, 0xD6, 0x44};

  EXPECT_EQ(itchbook::load_be32(ordered.data()), 0x01020304u);
  EXPECT_EQ(itchbook::load_be32(max.data()), 0xFFFFFFFFu);
  EXPECT_EQ(itchbook::load_be32(price.data()), 1234500u);
}

TEST(LoadBe, Be48) {
  const std::array<std::uint8_t, 6> ordered{0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  const std::array<std::uint8_t, 6> max{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  // 09:30:00 as nanoseconds since midnight: 34200000000000 = 0x1F1ACED9F000.
  const std::array<std::uint8_t, 6> open{0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00};

  EXPECT_EQ(itchbook::load_be48(ordered.data()), 0x010203040506u);
  EXPECT_EQ(itchbook::load_be48(max.data()), 0xFFFFFFFFFFFFu);
  EXPECT_EQ(itchbook::load_be48(open.data()), 34'200'000'000'000u);
}

TEST(LoadBe, Be64) {
  const std::array<std::uint8_t, 8> ordered{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  const std::array<std::uint8_t, 8> forty_two{0, 0, 0, 0, 0, 0, 0, 0x2A};
  const std::array<std::uint8_t, 8> high{0x80, 0, 0, 0, 0, 0, 0, 0x01};

  EXPECT_EQ(itchbook::load_be64(ordered.data()), 0x0102030405060708u);
  EXPECT_EQ(itchbook::load_be64(forty_two.data()), 42u);
  EXPECT_EQ(itchbook::load_be64(high.data()), 0x8000000000000001u);
}

TEST(LoadBe, ReadsAtOffset) {
  // An Add Order's order reference number starts at byte 11 of the message.
  std::array<std::uint8_t, 19> message{};
  message[0] = 'A';
  message[18] = 0x2A;

  EXPECT_EQ(itchbook::load_be64(message.data() + 11), 42u);
}

TEST(Messages, SideMatchesWireBytes) {
  EXPECT_EQ(static_cast<char>(itchbook::Side::Buy), 'B');
  EXPECT_EQ(static_cast<char>(itchbook::Side::Sell), 'S');
}

namespace {

// Builds a message: the type byte, a fixed header, then body. The header is
// stock locate 13, tracking number 2, timestamp 09:30:00 (34200000000000 ns).
std::vector<std::uint8_t> message(char type, std::initializer_list<std::uint8_t> body) {
  std::vector<std::uint8_t> m{static_cast<std::uint8_t>(type),
                              0x00, 0x0D,                           // stock locate
                              0x00, 0x02,                           // tracking number
                              0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00};  // timestamp
  m.insert(m.end(), body);
  return m;
}

}  // namespace

TEST(Decode, Header) {
  const auto m = message('D', {0, 0, 0, 0, 0, 0, 0, 0x2A});
  const auto h = itchbook::decode_header(m.data());

  EXPECT_EQ(h.type, 'D');
  EXPECT_EQ(h.stock_locate, 13u);
  EXPECT_EQ(h.tracking_number, 2u);
  EXPECT_EQ(h.timestamp, 34'200'000'000'000u);
}

TEST(Decode, SystemEvent) {
  const auto m = message('S', {'Q'});
  ASSERT_EQ(m.size(), itchbook::expected_lengths['S']);
  const auto msg = itchbook::decode_system_event(m.data());

  EXPECT_EQ(msg.header.stock_locate, 13u);
  EXPECT_EQ(msg.event_code, 'Q');
}

TEST(Decode, StockDirectory) {
  const auto m = message('R', {
      'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',  // stock
      'Q',                                     // market category
      'N',                                     // financial status
      0x00, 0x00, 0x00, 0x64,                  // round lot size 100
      'N', 'C', 'Z', ' ', 'P', 'N', ' ', '1', 'N', 0, 0, 0, 0, 'N',  // fields not decoded
  });
  ASSERT_EQ(m.size(), itchbook::expected_lengths['R']);
  const auto msg = itchbook::decode_stock_directory(m.data());

  EXPECT_EQ(msg.header.stock_locate, 13u);
  EXPECT_EQ(msg.stock, "AAPL");
  EXPECT_EQ(msg.market_category, 'Q');
  EXPECT_EQ(msg.financial_status, 'N');
  EXPECT_EQ(msg.round_lot_size, 100u);
}

TEST(Decode, AddOrder) {
  const auto m = message('A', {
      0, 0, 0, 0, 0, 0, 0, 0x2A,               // order ref 42
      'B',                                     // side
      0x00, 0x00, 0x00, 0x64,                  // shares 100
      'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',  // stock
      0x00, 0x12, 0xD6, 0x44,                  // price $123.4500
  });
  ASSERT_EQ(m.size(), itchbook::expected_lengths['A']);
  const auto msg = itchbook::decode_add_order(m.data());

  EXPECT_EQ(msg.header.timestamp, 34'200'000'000'000u);
  EXPECT_EQ(msg.order_ref, 42u);
  EXPECT_EQ(msg.side, itchbook::Side::Buy);
  EXPECT_EQ(msg.shares, 100u);
  EXPECT_EQ(msg.stock, "AAPL");
  EXPECT_EQ(msg.price, 1234500u);
  EXPECT_TRUE(msg.attribution.empty());
}

TEST(Decode, AddOrderWithAttribution) {
  const auto m = message('F', {
      0, 0, 0, 0, 0, 0, 0, 0x2A,               // order ref 42
      'S',                                     // side
      0x00, 0x00, 0x00, 0x64,                  // shares 100
      'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',  // stock
      0x00, 0x12, 0xD6, 0x44,                  // price $123.4500
      'G', 'S', 'C', 'O',                      // attribution
  });
  ASSERT_EQ(m.size(), itchbook::expected_lengths['F']);
  const auto msg = itchbook::decode_add_order(m.data());

  EXPECT_EQ(msg.order_ref, 42u);
  EXPECT_EQ(msg.side, itchbook::Side::Sell);
  EXPECT_EQ(msg.shares, 100u);
  EXPECT_EQ(msg.stock, "AAPL");
  EXPECT_EQ(msg.price, 1234500u);
  EXPECT_EQ(msg.attribution, "GSCO");
}

TEST(Decode, OrderExecuted) {
  const auto m = message('E', {
      0, 0, 0, 0, 0, 0, 0, 0x2A,           // order ref 42
      0x00, 0x00, 0x00, 0x32,              // executed shares 50
      0, 0, 0, 0, 0, 0, 0x30, 0x39,        // match number 12345
  });
  ASSERT_EQ(m.size(), itchbook::expected_lengths['E']);
  const auto msg = itchbook::decode_order_executed(m.data());

  EXPECT_EQ(msg.order_ref, 42u);
  EXPECT_EQ(msg.executed_shares, 50u);
  EXPECT_EQ(msg.match_number, 12345u);
}

TEST(Decode, OrderExecutedWithPrice) {
  const auto m = message('C', {
      0, 0, 0, 0, 0, 0, 0, 0x2A,           // order ref 42
      0x00, 0x00, 0x00, 0x32,              // executed shares 50
      0, 0, 0, 0, 0, 0, 0x30, 0x39,        // match number 12345
      'Y',                                 // printable
      0x00, 0x12, 0xD6, 0x44,              // execution price $123.4500
  });
  ASSERT_EQ(m.size(), itchbook::expected_lengths['C']);
  const auto msg = itchbook::decode_order_executed_with_price(m.data());

  EXPECT_EQ(msg.order_ref, 42u);
  EXPECT_EQ(msg.executed_shares, 50u);
  EXPECT_EQ(msg.match_number, 12345u);
  EXPECT_TRUE(msg.printable);
  EXPECT_EQ(msg.execution_price, 1234500u);
}

TEST(Decode, OrderCancel) {
  const auto m = message('X', {
      0, 0, 0, 0, 0, 0, 0, 0x2A,           // order ref 42
      0x00, 0x00, 0x00, 0x0A,              // cancelled shares 10
  });
  ASSERT_EQ(m.size(), itchbook::expected_lengths['X']);
  const auto msg = itchbook::decode_order_cancel(m.data());

  EXPECT_EQ(msg.order_ref, 42u);
  EXPECT_EQ(msg.cancelled_shares, 10u);
}

TEST(Decode, OrderDelete) {
  const auto m = message('D', {0, 0, 0, 0, 0, 0, 0, 0x2A});  // order ref 42
  ASSERT_EQ(m.size(), itchbook::expected_lengths['D']);
  const auto msg = itchbook::decode_order_delete(m.data());

  EXPECT_EQ(msg.header.stock_locate, 13u);
  EXPECT_EQ(msg.order_ref, 42u);
}

TEST(Decode, OrderReplace) {
  const auto m = message('U', {
      0, 0, 0, 0, 0, 0, 0, 0x2A,           // original order ref 42
      0, 0, 0, 0, 0, 0, 0, 0x2B,           // new order ref 43
      0x00, 0x00, 0x00, 0xC8,              // shares 200
      0x00, 0x12, 0xD6, 0x44,              // price $123.4500
  });
  ASSERT_EQ(m.size(), itchbook::expected_lengths['U']);
  const auto msg = itchbook::decode_order_replace(m.data());

  EXPECT_EQ(msg.original_order_ref, 42u);
  EXPECT_EQ(msg.new_order_ref, 43u);
  EXPECT_EQ(msg.shares, 200u);
  EXPECT_EQ(msg.price, 1234500u);
}

namespace {

// A message of the right length for its type, with the body left as zeros.
std::vector<std::uint8_t> blank(char type) {
  auto m = message(type, {});
  m.resize(itchbook::expected_lengths[static_cast<unsigned char>(type)]);
  return m;
}

// Records the name of each Handler function that parse_message calls.
struct RecordingHandler : itchbook::Handler {
  std::vector<std::string> calls;
  std::uint64_t last_order_ref = 0;

  void on_system_event(const itchbook::SystemEvent&) override { calls.push_back("system_event"); }
  void on_stock_directory(const itchbook::StockDirectory&) override {
    calls.push_back("stock_directory");
  }
  void on_add(const itchbook::AddOrder& m) override {
    calls.push_back("add");
    last_order_ref = m.order_ref;
  }
  void on_execute(const itchbook::OrderExecuted&) override { calls.push_back("execute"); }
  void on_execute_with_price(const itchbook::OrderExecutedWithPrice&) override {
    calls.push_back("execute_with_price");
  }
  void on_cancel(const itchbook::OrderCancel&) override { calls.push_back("cancel"); }
  void on_delete(const itchbook::OrderDelete&) override { calls.push_back("delete"); }
  void on_replace(const itchbook::OrderReplace&) override { calls.push_back("replace"); }
};

}  // namespace

TEST(Parse, CallsMatchingHandler) {
  const std::vector<std::pair<char, std::string>> cases{
      {'S', "system_event"}, {'R', "stock_directory"}, {'A', "add"},
      {'F', "add"},          {'E', "execute"},         {'C', "execute_with_price"},
      {'X', "cancel"},       {'D', "delete"},          {'U', "replace"},
  };
  for (const auto& [type, expected] : cases) {
    RecordingHandler h;
    const auto m = blank(type);

    EXPECT_TRUE(itchbook::parse_message(m.data(), m.size(), h)) << type;
    EXPECT_EQ(h.calls, std::vector<std::string>{expected}) << type;
  }
}

TEST(Parse, PassesDecodedFields) {
  RecordingHandler h;
  auto m = blank('A');
  m[18] = 0x2A;  // last byte of the order ref

  itchbook::parse_message(m.data(), m.size(), h);
  EXPECT_EQ(h.last_order_ref, 42u);
}

TEST(Parse, SkipsTypesWeDoNotDecode) {
  RecordingHandler h;
  const auto m = blank('H');  // trading action: in the spec, not used by the book

  EXPECT_FALSE(itchbook::parse_message(m.data(), m.size(), h));
  EXPECT_TRUE(h.calls.empty());
}

TEST(Parse, SkipsWrongLength) {
  RecordingHandler h;
  const auto m = blank('A');

  EXPECT_FALSE(itchbook::parse_message(m.data(), m.size() - 1, h));
  EXPECT_TRUE(h.calls.empty());
}

TEST(Parse, BaseHandlerIgnoresEverything) {
  itchbook::Handler h;
  const auto m = blank('D');

  EXPECT_TRUE(itchbook::parse_message(m.data(), m.size(), h));
}

TEST(Format, Timestamp) {
  EXPECT_EQ(itchbook::format_timestamp(0), "00:00:00.000000000");
  EXPECT_EQ(itchbook::format_timestamp(34'200'000'000'000), "09:30:00.000000000");
  // 1 hour, 2 minutes, 3 seconds, 4 nanoseconds: every field is padded.
  EXPECT_EQ(itchbook::format_timestamp(3'723'000'000'004), "01:02:03.000000004");
  // The last nanosecond before 16:00.
  EXPECT_EQ(itchbook::format_timestamp(57'599'999'999'999), "15:59:59.999999999");
}

TEST(Format, Price) {
  EXPECT_EQ(itchbook::format_price(1'234'500), "123.4500");
  EXPECT_EQ(itchbook::format_price(0), "0.0000");
  EXPECT_EQ(itchbook::format_price(1), "0.0001");
  EXPECT_EQ(itchbook::format_price(100), "0.0100");
  EXPECT_EQ(itchbook::format_price(4'294'967'295), "429496.7295");  // largest uint32_t
}

TEST(Format, TrimSymbol) {
  EXPECT_EQ(itchbook::trim_symbol("AAPL    "), "AAPL");
  EXPECT_EQ(itchbook::trim_symbol("GOOGL   "), "GOOGL");
  EXPECT_EQ(itchbook::trim_symbol("ABCDEFGH"), "ABCDEFGH");  // full width, nothing to trim
  EXPECT_EQ(itchbook::trim_symbol("AB C    "), "AB C");      // only trailing spaces go
  EXPECT_EQ(itchbook::trim_symbol("        "), "");
}

TEST(Dump, PrintsAddOrder) {
  const auto m = message('A', {
      0, 0, 0, 0, 0, 0, 0, 0x2A,               // order ref 42
      'B',                                     // side
      0x00, 0x00, 0x00, 0x64,                  // shares 100
      'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',  // stock
      0x00, 0x12, 0xD6, 0x44,                  // price $123.4500
  });
  std::ostringstream out;
  itchbook::DumpHandler h(out, 0, 0);

  itchbook::parse_message(m.data(), m.size(), h);
  EXPECT_EQ(out.str(),
            "09:30:00.000000000 A locate=13 ref=42 side=B shares=100 stock=AAPL price=123.4500\n");
}

TEST(Dump, FiltersByType) {
  std::ostringstream out;
  itchbook::DumpHandler h(out, 'D', 0);

  for (const char type : {'A', 'D', 'F', 'E'}) {
    const auto m = blank(type);
    itchbook::parse_message(m.data(), m.size(), h);
  }
  EXPECT_EQ(out.str(), "09:30:00.000000000 D locate=13 ref=0\n");
  EXPECT_EQ(h.printed(), 1u);
}

TEST(Dump, DoneAtLimit) {
  std::ostringstream out;
  itchbook::DumpHandler h(out, 0, 2);
  const auto m = blank('D');

  EXPECT_FALSE(h.done());
  itchbook::parse_message(m.data(), m.size(), h);
  EXPECT_FALSE(h.done());
  itchbook::parse_message(m.data(), m.size(), h);
  EXPECT_TRUE(h.done());
}

TEST(Dump, ParseFileStopsWhenDone) {
  // Three D messages, each with its 2-byte length prefix.
  std::vector<std::uint8_t> bytes;
  for (int i = 0; i < 3; ++i) {
    const auto m = blank('D');
    bytes.push_back(0x00);
    bytes.push_back(static_cast<std::uint8_t>(m.size()));
    bytes.insert(bytes.end(), m.begin(), m.end());
  }
  const auto path = write_bytes("itchbook-parse-file-limit", bytes);
  std::ostringstream out;
  itchbook::DumpHandler h(out, 0, 2);

  itchbook::parse_file(path, h);
  std::filesystem::remove(path);
  EXPECT_EQ(h.printed(), 2u);
}

TEST(Symbols, AddAndLookUp) {
  itchbook::SymbolDirectory symbols;
  symbols.add(1, "A");
  symbols.add(2, "AA");

  EXPECT_EQ(symbols.symbol(1), "A");
  EXPECT_EQ(symbols.symbol(2), "AA");
  EXPECT_EQ(symbols.size(), 2u);
}

TEST(Symbols, UnknownLocateIsEmpty) {
  itchbook::SymbolDirectory symbols;
  symbols.add(3, "AAAU");

  EXPECT_EQ(symbols.symbol(0), "");    // slot 0 is never used
  EXPECT_EQ(symbols.symbol(1), "");    // inside the vector, never named
  EXPECT_EQ(symbols.symbol(500), "");  // past the end of the vector
}

TEST(Symbols, KeepsItsOwnCopy) {
  itchbook::SymbolDirectory symbols;
  std::string buf = "AAPL";
  symbols.add(13, buf);
  buf = "ZZZZ";  // what happens to the message buffer on the next message

  EXPECT_EQ(symbols.symbol(13), "AAPL");
}

TEST(Symbols, FromDecodedStockDirectory) {
  const auto m = message('R', {
      'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',  // stock
      'Q', 'N', 0x00, 0x00, 0x00, 0x64,        // category, status, round lot
      'N', 'C', 'Z', ' ', 'P', 'N', ' ', '1', 'N', 0, 0, 0, 0, 'N',
  });
  const auto msg = itchbook::decode_stock_directory(m.data());
  itchbook::SymbolDirectory symbols;

  symbols.add(msg.header.stock_locate, msg.stock);
  EXPECT_EQ(symbols.symbol(13), "AAPL");
}

TEST(Parse, OnMessageSeesEveryValidMessage) {
  struct CountingHandler : itchbook::Handler {
    int seen = 0;
    void on_message(const itchbook::MessageHeader&) override { ++seen; }
  } h;
  const auto add = blank('A');
  const auto trading_action = blank('H');

  itchbook::parse_message(add.data(), add.size(), h);
  itchbook::parse_message(trading_action.data(), trading_action.size(), h);  // not decoded, still seen
  itchbook::parse_message(add.data(), add.size() - 1, h);                    // wrong length, not seen
  EXPECT_EQ(h.seen, 2);
}

namespace {

// Writes value into m at offset as a big-endian number `bytes` wide: the
// reverse of load_be*.
void put_be(std::vector<std::uint8_t>& m, std::size_t offset, std::uint64_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) {
    m[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value & 0xFF);
    value >>= 8;
  }
}

// A blank message of `type` for stock `locate`, sent at `second` past midnight.
std::vector<std::uint8_t> at(char type, std::uint16_t locate, std::uint64_t second) {
  auto m = blank(type);
  put_be(m, 1, locate, 2);
  put_be(m, 5, second * 1'000'000'000, 6);
  return m;
}

void feed(itchbook::Handler& h, const std::vector<std::uint8_t>& m) {
  itchbook::parse_message(m.data(), m.size(), h);
}

}  // namespace

TEST(Stats, CountsByType) {
  itchbook::StatsHandler stats;
  for (const char type : {'A', 'A', 'D', 'H'}) {
    feed(stats, blank(type));
  }

  EXPECT_EQ(stats.messages(), 4u);
  EXPECT_EQ(stats.by_type()['A'], 2u);
  EXPECT_EQ(stats.by_type()['D'], 1u);
  EXPECT_EQ(stats.by_type()['H'], 1u);
}

TEST(Stats, TopSymbolsSortedAndLimited) {
  itchbook::StatsHandler stats;
  auto r = message('R', {
      'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',
      'Q', 'N', 0x00, 0x00, 0x00, 0x64,
      'N', 'C', 'Z', ' ', 'P', 'N', ' ', '1', 'N', 0, 0, 0, 0, 'N',
  });
  put_be(r, 1, 2, 2);  // AAPL is locate 2
  feed(stats, r);
  feed(stats, at('A', 1, 0));
  feed(stats, at('A', 2, 0));
  feed(stats, at('A', 2, 0));
  feed(stats, at('A', 3, 0));
  feed(stats, at('A', 3, 0));
  feed(stats, at('S', 0, 0));  // locate 0 is the system, not a stock

  const auto top = stats.top_symbols(2);
  ASSERT_EQ(top.size(), 2u);
  EXPECT_EQ(top[0].locate, 2u);  // R + 2 adds = 3 messages
  EXPECT_EQ(top[0].symbol, "AAPL");
  EXPECT_EQ(top[0].messages, 3u);
  EXPECT_EQ(top[1].locate, 3u);  // 2 messages
  EXPECT_EQ(top[1].symbol, "");   // no R message named it
  EXPECT_EQ(top[1].messages, 2u);
  EXPECT_EQ(stats.symbols().size(), 1u);
}

TEST(Stats, BusiestSecond) {
  itchbook::StatsHandler stats;
  feed(stats, at('D', 1, 34'200));
  feed(stats, at('D', 1, 34'201));
  feed(stats, at('D', 1, 34'201));
  feed(stats, at('D', 1, 34'205));

  const auto busiest = stats.busiest_second();
  EXPECT_EQ(busiest.second, 34'201u);  // 09:30:01
  EXPECT_EQ(busiest.messages, 2u);
}

TEST(Stats, LargestOrderRefIncludesReplaces) {
  itchbook::StatsHandler stats;
  auto add = blank('A');
  put_be(add, 11, 42, 8);
  auto replace = blank('U');
  put_be(replace, 19, 100, 8);  // new order ref
  auto small_add = blank('A');
  put_be(small_add, 11, 7, 8);

  feed(stats, add);
  feed(stats, replace);
  feed(stats, small_add);
  EXPECT_EQ(stats.max_order_ref(), 100u);
}

namespace {

// Book messages with only the fields the book reads filled in. Offsets are
// the ones in messages.hpp.
std::vector<std::uint8_t> add(std::uint64_t ref, char side, std::uint32_t shares,
                              std::uint32_t price, std::uint16_t locate = 1) {
  auto m = blank('A');
  put_be(m, 1, locate, 2);
  put_be(m, 11, ref, 8);
  m[19] = static_cast<std::uint8_t>(side);
  put_be(m, 20, shares, 4);
  put_be(m, 32, price, 4);
  return m;
}

std::vector<std::uint8_t> execute(std::uint64_t ref, std::uint32_t shares) {
  auto m = blank('E');
  put_be(m, 11, ref, 8);
  put_be(m, 19, shares, 4);
  return m;
}

std::vector<std::uint8_t> execute_with_price(std::uint64_t ref, std::uint32_t shares,
                                             std::uint32_t price) {
  auto m = blank('C');
  put_be(m, 11, ref, 8);
  put_be(m, 19, shares, 4);
  m[31] = 'Y';
  put_be(m, 32, price, 4);
  return m;
}

std::vector<std::uint8_t> cancel(std::uint64_t ref, std::uint32_t shares) {
  auto m = blank('X');
  put_be(m, 11, ref, 8);
  put_be(m, 19, shares, 4);
  return m;
}

std::vector<std::uint8_t> remove(std::uint64_t ref) {
  auto m = blank('D');
  put_be(m, 11, ref, 8);
  return m;
}

std::vector<std::uint8_t> replace(std::uint64_t old_ref, std::uint64_t new_ref,
                                  std::uint32_t shares, std::uint32_t price) {
  auto m = blank('U');
  put_be(m, 11, old_ref, 8);
  put_be(m, 19, new_ref, 8);
  put_be(m, 27, shares, 4);
  put_be(m, 31, price, 4);
  return m;
}

using Levels = std::vector<itchbook::Level>;

}  // namespace

// Every book test runs once per book type: BookV2 must behave exactly like
// BookV1. Inside a TYPED_TEST, TypeParam is the book type being tested.
template <class Book>
class BookTest : public ::testing::Test {};
template <class Book>
class BookDeathTest : public ::testing::Test {};

using BookTypes =
    ::testing::Types<itchbook::BookV1, itchbook::BookV2, itchbook::BookV2Std, itchbook::BookV2Direct>;

// Names the runs "v1" and "v2" in test output instead of the full type names.
struct BookNames {
  template <class Book>
  static std::string GetName(int) {
    if constexpr (std::is_same_v<Book, itchbook::BookV1>) {
      return "v1";
    } else if constexpr (std::is_same_v<Book, itchbook::BookV2Std>) {
      return "v2_std";
    } else if constexpr (std::is_same_v<Book, itchbook::BookV2Direct>) {
      return "v2_direct";
    } else {
      return "v2";
    }
  }
};

TYPED_TEST_SUITE(BookTest, BookTypes, BookNames);
TYPED_TEST_SUITE(BookDeathTest, BookTypes, BookNames);

TYPED_TEST(BookTest, AddBuildsLevelsBestFirst) {
  TypeParam book;
  feed(book, add(1, 'B', 10, 100));
  feed(book, add(2, 'B', 5, 100));  // same price: joins the level
  feed(book, add(3, 'B', 7, 101));
  feed(book, add(4, 'S', 4, 103));
  feed(book, add(5, 'S', 6, 102));

  const auto top = book.top(1, 5);
  EXPECT_EQ(top.bids, (Levels{{101, 7}, {100, 15}}));  // highest bid first
  EXPECT_EQ(top.asks, (Levels{{102, 6}, {103, 4}}));   // lowest ask first
  EXPECT_EQ(book.order_count(), 5u);
}

TYPED_TEST(BookTest, TopLimitsDepth) {
  TypeParam book;
  feed(book, add(1, 'B', 10, 100));
  feed(book, add(2, 'B', 10, 99));
  feed(book, add(3, 'B', 10, 98));

  EXPECT_EQ(book.top(1, 2).bids, (Levels{{100, 10}, {99, 10}}));
}

TYPED_TEST(BookTest, ExecuteReducesThenRemoves) {
  TypeParam book;
  feed(book, add(1, 'B', 10, 100));

  feed(book, execute(1, 4));
  EXPECT_EQ(book.top(1, 5).bids, (Levels{{100, 6}}));
  EXPECT_EQ(book.order_count(), 1u);

  feed(book, execute(1, 6));  // the rest: order and level both go
  EXPECT_TRUE(book.top(1, 5).bids.empty());
  EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(BookTest, ExecuteWithPriceUsesOrderPrice) {
  TypeParam book;
  feed(book, add(1, 'S', 10, 100));
  feed(book, execute_with_price(1, 4, 105));  // traded at 105, but rests at 100

  EXPECT_EQ(book.top(1, 5).asks, (Levels{{100, 6}}));
}

TYPED_TEST(BookTest, CancelReducesShares) {
  TypeParam book;
  feed(book, add(1, 'B', 10, 100));
  feed(book, cancel(1, 3));

  EXPECT_EQ(book.top(1, 5).bids, (Levels{{100, 7}}));
  EXPECT_EQ(book.order_count(), 1u);
}

TYPED_TEST(BookTest, DeleteRemovesOnlyThatOrder) {
  TypeParam book;
  feed(book, add(1, 'B', 10, 100));
  feed(book, add(2, 'B', 5, 100));
  feed(book, remove(1));

  EXPECT_EQ(book.top(1, 5).bids, (Levels{{100, 5}}));
  EXPECT_EQ(book.order_count(), 1u);
}

TYPED_TEST(BookTest, ReplaceKeepsLocateAndSide) {
  TypeParam book;
  feed(book, add(1, 'S', 10, 100, 7));
  feed(book, replace(1, 2, 20, 101));

  const auto top = book.top(7, 5);
  EXPECT_TRUE(top.bids.empty());
  EXPECT_EQ(top.asks, (Levels{{101, 20}}));  // old level 100 is gone
  EXPECT_EQ(book.order_count(), 1u);

  feed(book, execute(2, 5));  // the new ref is live
  EXPECT_EQ(book.top(7, 5).asks, (Levels{{101, 15}}));
  EXPECT_EQ(book.errors(), 0u);

  feed(book, execute(1, 5));  // the old ref is not
  EXPECT_EQ(book.errors(), 1u);
}

TYPED_TEST(BookTest, BooksAreSeparatePerStock) {
  TypeParam book;
  feed(book, add(1, 'B', 10, 100, 1));
  feed(book, add(2, 'B', 20, 200, 2));

  EXPECT_EQ(book.top(1, 5).bids, (Levels{{100, 10}}));
  EXPECT_EQ(book.top(2, 5).bids, (Levels{{200, 20}}));
  EXPECT_TRUE(book.top(3, 5).bids.empty());  // never seen
}

TYPED_TEST(BookTest, UnknownOrDuplicateRefIsAnError) {
  TypeParam book;
  feed(book, execute(99, 1));
  feed(book, remove(99));
  feed(book, add(1, 'B', 10, 100));
  feed(book, add(1, 'B', 10, 100));  // same ref again

  EXPECT_EQ(book.errors(), 3u);
  EXPECT_EQ(book.top(1, 5).bids, (Levels{{100, 10}}));  // not double counted
}

// Death tests run in a child process, so a failed assert ends the child, not
// the test run. EXPECT_DEBUG_DEATH only expects a death in Debug builds,
// where assert is on; in Release it just runs the statement.
TYPED_TEST(BookDeathTest, ExecuteMoreThanOrderAsserts) {
  TypeParam book;
  feed(book, add(1, 'B', 10, 100));
  EXPECT_DEBUG_DEATH(feed(book, execute(1, 11)), "shares <= order");
}

TYPED_TEST(BookDeathTest, CancelMoreThanOrderAsserts) {
  TypeParam book;
  feed(book, add(1, 'S', 10, 100));
  EXPECT_DEBUG_DEATH(feed(book, cancel(1, 11)), "shares <= order");
}

namespace {

// One step of a scripted sequence: a message, then both sides of locate 1's
// book as they should look after it.
struct ScriptStep {
  std::vector<std::uint8_t> message;
  Levels bids;
  Levels asks;
};

template <class Book>
void run_script(const std::vector<ScriptStep>& script, std::uint16_t locate = 1) {
  Book book;
  for (std::size_t i = 0; i < script.size(); ++i) {
    feed(book, script[i].message);
    const auto top = book.top(locate, 10);
    EXPECT_EQ(top.bids, script[i].bids) << "bids after step " << i;
    EXPECT_EQ(top.asks, script[i].asks) << "asks after step " << i;
  }
  EXPECT_EQ(book.errors(), 0u);
}

}  // namespace

TYPED_TEST(BookTest, ScriptAddPartialExecuteReplaceDelete) {
  run_script<TypeParam>({
      {add(1, 'B', 100, 10'000), {{10'000, 100}}, {}},
      {add(2, 'B', 50, 10'000), {{10'000, 150}}, {}},
      {add(3, 'S', 200, 10'100), {{10'000, 150}}, {{10'100, 200}}},
      {execute(1, 30), {{10'000, 120}}, {{10'100, 200}}},           // partial execute
      {replace(3, 4, 80, 10'050), {{10'000, 120}}, {{10'050, 80}}},  // ask moves in
      {add(5, 'B', 10, 9'900), {{10'000, 120}, {9'900, 10}}, {{10'050, 80}}},
      {cancel(2, 50), {{10'000, 70}, {9'900, 10}}, {{10'050, 80}}},  // cancel all of order 2
      {remove(1), {{9'900, 10}}, {{10'050, 80}}},                    // best bid level empties
      {execute_with_price(4, 80, 10'060), {{9'900, 10}}, {}},        // fills at its own level
      {remove(5), {}, {}},
  });
}

TYPED_TEST(BookTest, ScriptReplaceToSameLevelAndBack) {
  run_script<TypeParam>({
      {add(1, 'S', 100, 5'000), {}, {{5'000, 100}}},
      {add(2, 'S', 100, 5'000), {}, {{5'000, 200}}},
      {replace(1, 3, 40, 5'000), {}, {{5'000, 140}}},  // same price, fewer shares
      {replace(3, 4, 40, 5'100), {}, {{5'000, 100}, {5'100, 40}}},
      {replace(4, 5, 60, 5'000), {}, {{5'000, 160}}},
      {execute(2, 100), {}, {{5'000, 60}}},
  });
}

// The real ARGX orders from small.itch: order 8913 is replaced by 32997, one
// cent higher. Prices have 4 implied decimals.
TYPED_TEST(BookTest, ScriptArgxReplaceFromRealData) {
  run_script<TypeParam>(
      {
          {add(8881, 'B', 2000, 1'605'800, 496), {{1'605'800, 2000}}, {}},
          {add(8913, 'S', 400, 1'624'700, 496), {{1'605'800, 2000}}, {{1'624'700, 400}}},
          {add(8933, 'S', 2000, 1'634'600, 496),
           {{1'605'800, 2000}},
           {{1'624'700, 400}, {1'634'600, 2000}}},
          {replace(8913, 32997, 400, 1'624'800),
           {{1'605'800, 2000}},
           {{1'624'800, 400}, {1'634'600, 2000}}},
      },
      496);
}

TYPED_TEST(BookTest, PeakOrderCount) {
  TypeParam book;
  feed(book, add(1, 'B', 10, 100));
  feed(book, add(2, 'B', 10, 100));
  feed(book, add(3, 'B', 10, 100));
  feed(book, remove(1));
  feed(book, add(4, 'B', 10, 100));

  EXPECT_EQ(book.order_count(), 3u);
  EXPECT_EQ(book.peak_order_count(), 3u);

  feed(book, add(5, 'B', 10, 100));
  EXPECT_EQ(book.peak_order_count(), 4u);
}

TYPED_TEST(BookTest, State) {
  TypeParam book;
  EXPECT_EQ(book.state(1), itchbook::BookState::Normal);  // no book at all

  feed(book, add(1, 'B', 10, 100));
  EXPECT_EQ(book.state(1), itchbook::BookState::Normal);  // one side only

  feed(book, add(2, 'S', 10, 101));
  EXPECT_EQ(book.state(1), itchbook::BookState::Normal);

  feed(book, add(3, 'S', 10, 100));
  EXPECT_EQ(book.state(1), itchbook::BookState::Locked);

  feed(book, add(4, 'S', 10, 99));
  EXPECT_EQ(book.state(1), itchbook::BookState::Crossed);
}

TEST(Stats, PeakLiveOrders) {
  itchbook::StatsHandler stats;
  feed(stats, add(1, 'B', 10, 100));
  feed(stats, add(2, 'S', 10, 101));
  feed(stats, remove(1));
  feed(stats, add(3, 'B', 10, 100));

  EXPECT_EQ(stats.peak_order_count(), 2u);
}

TEST(Symbols, FindByName) {
  itchbook::SymbolDirectory symbols;
  symbols.add(1, "A");
  symbols.add(2, "AA");
  symbols.add(5, "AAPL");

  EXPECT_EQ(symbols.find("AA"), 2u);
  EXPECT_EQ(symbols.find("AAPL"), 5u);
  EXPECT_EQ(symbols.find("MSFT"), std::nullopt);
  EXPECT_EQ(symbols.find(""), std::nullopt);  // not one of the unnamed slots
}

TEST(Format, ParseTimestamp) {
  EXPECT_EQ(itchbook::parse_timestamp("00:00:00"), 0u);
  EXPECT_EQ(itchbook::parse_timestamp("10:30:00"), 37'800'000'000'000u);
  EXPECT_EQ(itchbook::parse_timestamp("09:30:00.5"), 34'200'500'000'000u);
  EXPECT_EQ(itchbook::parse_timestamp("15:59:59.999999999"), 57'599'999'999'999u);

  for (const char* bad : {"24:00:00", "10:60:00", "10:30:60", "9:30:00", "10:30", "10:30:00.",
                          "10:30:00.1234567890", "10-30-00", "ab:cd:ef", "10:30:00x"}) {
    EXPECT_EQ(itchbook::parse_timestamp(bad), std::nullopt) << bad;
  }
}

TEST(Format, ParseTimestampRoundTrips) {
  const std::uint64_t t = 45'296'123'456'789;  // 12:34:56.123456789
  EXPECT_EQ(itchbook::parse_timestamp(itchbook::format_timestamp(t)), t);
}

namespace {

// Sets a message's timestamp to `second` past midnight.
std::vector<std::uint8_t> stamp(std::vector<std::uint8_t> m, std::uint64_t second) {
  put_be(m, 5, second * 1'000'000'000, 6);
  return m;
}

// An R message naming locate 13 "AAPL".
std::vector<std::uint8_t> aapl_directory() {
  return message('R', {
      'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',
      'Q', 'N', 0x00, 0x00, 0x00, 0x64,
      'N', 'C', 'Z', ' ', 'P', 'N', ' ', '1', 'N', 0, 0, 0, 0, 'N',
  });
}

}  // namespace

TEST(CheckedBook, CountsLockedAndCrossedInMarketHours) {
  itchbook::CheckedBook book;
  feed(book, aapl_directory());
  feed(book, stamp(add(1, 'B', 10, 100, 13), 36'000));  // 10:00:00
  feed(book, stamp(add(2, 'S', 10, 100, 13), 36'001));  // locked
  feed(book, stamp(add(3, 'S', 10, 99, 13), 36'002));   // crossed
  feed(book, stamp(remove(3), 36'003));                 // locked again (D is locate 13 too)

  EXPECT_EQ(book.messages(), 5u);
  EXPECT_EQ(book.locked_moments(), 2u);
  EXPECT_EQ(book.crossed_moments(), 1u);

  const auto worst = book.worst(10);
  ASSERT_EQ(worst.size(), 1u);
  EXPECT_EQ(worst[0].symbol, "AAPL");
  EXPECT_EQ(worst[0].locked, 2u);
  EXPECT_EQ(worst[0].crossed, 1u);
  EXPECT_EQ(worst[0].first_time, 36'001'000'000'000u);
  EXPECT_EQ(worst[0].last_time, 36'003'000'000'000u);
}

TEST(CheckedBook, IgnoresOutsideMarketHours) {
  itchbook::CheckedBook book;
  feed(book, stamp(add(1, 'B', 10, 100), 28'800));  // 08:00:00
  feed(book, stamp(add(2, 'S', 10, 99), 28'801));   // crossed, but pre-market
  feed(book, stamp(add(3, 'S', 10, 98), 57'600));   // 16:00:00, already closed

  EXPECT_EQ(book.locked_moments() + book.crossed_moments(), 0u);
}

TEST(BookAt, SnapshotBeforeFirstLaterMessage) {
  itchbook::BookAt book("AAPL", 34'250'000'000'000, 5);
  feed(book, aapl_directory());                         // 09:30:00
  feed(book, stamp(add(1, 'B', 10, 100, 13), 34'250));  // exactly at the time: included
  EXPECT_FALSE(book.done());
  feed(book, stamp(add(2, 'B', 5, 101, 13), 34'300));   // after the time: not included

  EXPECT_TRUE(book.done());
  EXPECT_EQ(book.locate(), 13u);
  EXPECT_EQ(book.result().bids, (Levels{{100, 10}}));
}

TEST(BookAt, FileEndsBeforeTime) {
  itchbook::BookAt book("AAPL", 50'000'000'000'000, 5);
  feed(book, aapl_directory());
  feed(book, stamp(add(1, 'B', 10, 100, 13), 34'250));

  EXPECT_FALSE(book.done());
  EXPECT_EQ(book.result().bids, (Levels{{100, 10}}));  // the final book
}

TEST(GoldenWriter, SnapshotsEveryInterval) {
  std::ostringstream out;
  itchbook::GoldenWriter golden(out, {"AAPL"}, 2);
  feed(golden, aapl_directory());                                // 09:30:00
  feed(golden, stamp(add(1, 'B', 10, 1'000'000, 13), 34'210));   // 09:30:10
  feed(golden, stamp(add(2, 'S', 20, 1'010'000, 13), 34'270));   // 09:31:10
  feed(golden, stamp(remove(1), 34'400));                        // 09:33:20

  EXPECT_EQ(out.str(),
            "time,symbol,bid_price_1,bid_shares_1,bid_price_2,bid_shares_2,"
            "ask_price_1,ask_shares_1,ask_price_2,ask_shares_2\n"
            "09:30:00,AAPL,,,,,,,,\n"
            "09:31:00,AAPL,100.0000,10,,,,,,\n"
            "09:32:00,AAPL,100.0000,10,,,101.0000,20,,\n"
            "09:33:00,AAPL,100.0000,10,,,101.0000,20,,\n");
  EXPECT_EQ(golden.rows(), 4u);
}

TEST(Ticker, FiresForEachTimeAMessagePasses) {
  itchbook::Ticker ticker(10);
  std::vector<std::uint64_t> ticks;
  const auto record = [&](std::uint64_t t) { ticks.push_back(t); };

  ticker.advance(15, record);  // first tick: the first multiple of 10 at or after 15
  ticker.advance(20, record);  // stamped exactly at the tick: not past it yet
  ticker.advance(45, record);  // passes 20, 30 and 40
  EXPECT_EQ(ticks, (std::vector<std::uint64_t>{20, 30, 40}));
}

TEST(TopOfBookSeries, SamplesBestLevelEachTick) {
  itchbook::TopOfBookSeries series("AAPL", 60'000'000'000);
  feed(series, aapl_directory());                                // 09:30:00
  feed(series, stamp(add(1, 'B', 10, 1'000'000, 13), 34'210));   // 09:30:10
  feed(series, stamp(add(2, 'B', 5, 1'000'100, 13), 34'230));    // better bid
  feed(series, stamp(add(3, 'S', 20, 1'010'000, 13), 34'270));   // 09:31:10
  feed(series, stamp(remove(3), 34'330));                        // 09:32:10

  const auto& c = series.columns();
  EXPECT_EQ(c.time, (std::vector<std::uint64_t>{34'200'000'000'000, 34'260'000'000'000,
                                                34'320'000'000'000}));
  EXPECT_EQ(c.bid_price, (std::vector<std::uint32_t>{0, 1'000'100, 1'000'100}));
  EXPECT_EQ(c.bid_shares, (std::vector<std::uint64_t>{0, 5, 5}));
  EXPECT_EQ(c.ask_price, (std::vector<std::uint32_t>{0, 0, 1'010'000}));
  EXPECT_EQ(c.ask_shares, (std::vector<std::uint64_t>{0, 0, 20}));
  EXPECT_EQ(series.locate(), 13u);
}

namespace {

// Writes messages to a temp file, each with its 2-byte length prefix.
std::filesystem::path write_messages(const char* name,
                                     const std::vector<std::vector<std::uint8_t>>& messages) {
  std::vector<std::uint8_t> bytes;
  for (const auto& m : messages) {
    bytes.push_back(static_cast<std::uint8_t>(m.size() >> 8));
    bytes.push_back(static_cast<std::uint8_t>(m.size() & 0xFF));
    bytes.insert(bytes.end(), m.begin(), m.end());
  }
  return write_bytes(name, bytes);
}

}  // namespace

TEST(MessageReader, ReportsOffsetsAndResumes) {
  const auto path = write_messages("itchbook-reader", {blank('D'), blank('A'), blank('X')});
  {
    const itchbook::MappedFile file(path);
    itchbook::MessageReader reader(file.bytes());
    std::vector<std::uint64_t> offsets;
    while (reader.next()) {
      offsets.push_back(reader.offset());
    }
    // Each message is its 2-byte prefix plus its spec length: D 19, A 36, X 23.
    EXPECT_EQ(offsets, (std::vector<std::uint64_t>{0, 21, 59}));
    EXPECT_FALSE(reader.truncated());

    itchbook::MessageReader resumed(file.bytes(), 21);
    ASSERT_TRUE(resumed.next());
    EXPECT_EQ(resumed.data()[0], 'A');
    EXPECT_EQ(resumed.size(), 36u);
    EXPECT_EQ(resumed.offset(), 21u);
    // Zero-copy: the message is the mapped bytes themselves, just past the prefix.
    EXPECT_EQ(resumed.data(), file.data() + 21 + 2);
  }
  std::filesystem::remove(path);
}

TEST(MessageReader, StopsAtACutOffMessage) {
  // A whole D, then a length prefix promising 36 bytes with only 3 behind it.
  std::vector<std::uint8_t> bytes{0x00, 19};
  const auto d = blank('D');
  bytes.insert(bytes.end(), d.begin(), d.end());
  bytes.insert(bytes.end(), {0x00, 36, 'A', 0, 0});

  itchbook::MessageReader reader(bytes);
  ASSERT_TRUE(reader.next());
  EXPECT_EQ(reader.data()[0], 'D');
  EXPECT_FALSE(reader.next());
  EXPECT_TRUE(reader.truncated());
}

TEST(MessageReader, OffsetPastTheEndReadsNothing) {
  const std::vector<std::uint8_t> bytes{0x00, 0x01, 'Z'};
  itchbook::MessageReader reader(bytes, 1'000);
  EXPECT_FALSE(reader.next());
  EXPECT_FALSE(reader.truncated());
}

TEST(MappedFile, RefusesWhatIsNotARegularFile) {
  // /dev/null, like a pipe, reports size 0: mapping it would quietly read nothing.
  EXPECT_THROW(itchbook::MappedFile("/dev/null"), std::filesystem::filesystem_error);
}

TEST(MappedFile, ClosesItsFileDescriptor) {
  // Map more times than this process may have files open. If the destructor
  // leaked its descriptor, open() would start failing partway through.
  rlimit limit{};
  ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &limit), 0);
  const rlim_t times = std::min<rlim_t>(limit.rlim_cur, 100'000) + 100;
  const auto path = write_bytes("itchbook-mapped-file-fd", {0x00, 0x01, 'Z'});
  for (rlim_t i = 0; i < times; ++i) {
    const itchbook::MappedFile file(path);
    ASSERT_EQ(file.size(), 3u) << "after " << i << " maps";
  }
  std::filesystem::remove(path);
}

namespace {

// A short AAPL day (locate 13) with another stock (locate 14) mixed in.
std::vector<std::vector<std::uint8_t>> snapshot_day() {
  return {
      aapl_directory(),                                          // 09:30:00
      stamp(add(1, 'B', 10, 1'000'000, 13), 34'210),
      stamp(add(2, 'S', 20, 1'010'000, 13), 34'230),
      stamp(add(3, 'B', 7, 900'000, 14), 34'240),                // other stock
      stamp(execute(1, 4), 34'290),
      stamp(replace(2, 4, 15, 1'005'000), 34'350),
      stamp(remove(4), 34'420),
      stamp(add(5, 'B', 3, 1'001'000, 13), 34'500),
  };
}

// The answer a plain replay from the start gives.
itchbook::BookTop replay_to(const std::filesystem::path& path, std::uint64_t second) {
  itchbook::BookAt book("AAPL", second * 1'000'000'000, 5);
  itchbook::parse_file(path, book);
  return book.result();
}

}  // namespace

TEST(SnapshotIndex, MatchesFullReplayForEveryInterval) {
  const auto path = write_messages("itchbook-snapshot", snapshot_day());
  const std::vector<std::uint64_t> seconds{34'200, 34'215, 34'260, 34'290, 34'300,
                                           34'359, 34'400, 34'450, 34'600};

  for (const std::uint64_t interval : {0ull, 1ull, 30ull, 60ull, 3600ull}) {
    const itchbook::SnapshotIndex index(path, {"AAPL", "NOPE"}, interval * 1'000'000'000);
    EXPECT_EQ(index.symbols(), std::vector<std::string>{"AAPL"});
    for (const std::uint64_t s : seconds) {
      const auto got = index.book("AAPL", s * 1'000'000'000, 5);
      ASSERT_TRUE(got.has_value());
      const auto want = replay_to(path, s);
      EXPECT_EQ(got->bids, want.bids) << "interval " << interval << " at " << s;
      EXPECT_EQ(got->asks, want.asks) << "interval " << interval << " at " << s;
    }
  }
  std::filesystem::remove(path);
}

TEST(SnapshotIndex, CountsSnapshotsAndMessages) {
  const auto path = write_messages("itchbook-snapshot-stats", snapshot_day());
  const itchbook::SnapshotIndex index(path, {"AAPL"}, 60'000'000'000);

  EXPECT_EQ(index.stats().messages, 8u);
  EXPECT_EQ(index.stats().by_type['A'], 4u);
  // Ticks at 09:30, 09:31, 09:32, 09:33 and 09:34; the last message is 09:35.
  EXPECT_EQ(index.stats().snapshots, 5u);
  EXPECT_FALSE(index.book("MSFT", 34'300'000'000'000, 5).has_value());
  std::filesystem::remove(path);
}

TEST(SnapshotIndex, BboSamplesEachInterval) {
  const auto path = write_messages("itchbook-snapshot-bbo", snapshot_day());
  const itchbook::SnapshotIndex index(path, {"AAPL"}, 60'000'000'000);

  const auto bbo = index.bbo("AAPL", 34'260'000'000'000, 34'440'000'000'000, 60'000'000'000);
  ASSERT_TRUE(bbo.has_value());
  EXPECT_EQ(bbo->time.size(), 4u);  // 09:31, 09:32, 09:33, 09:34
  for (std::size_t i = 0; i < bbo->time.size(); ++i) {
    const auto want = replay_to(path, bbo->time[i] / 1'000'000'000);
    EXPECT_EQ(bbo->bid_price[i], want.bids.empty() ? 0 : want.bids[0].price) << i;
    EXPECT_EQ(bbo->ask_price[i], want.asks.empty() ? 0 : want.asks[0].price) << i;
  }
  std::filesystem::remove(path);
}

TEST(Core, ReadMessagesStopsWhenCutOff) {
  const auto path = write_bytes("itchbook-read-messages-cutoff", {0x00, 0x04, 'A', 'B'});
  const auto messages = itchbook::read_messages(path);
  std::filesystem::remove(path);

  EXPECT_TRUE(messages.empty());
}

TEST(MappedFile, MapsTheFileBytes) {
  const std::vector<std::uint8_t> bytes = {0x00, 0x03, 'A', 'B', 'C'};
  const auto path = write_bytes("itchbook-mapped-file", bytes);
  {
    const itchbook::MappedFile file(path);
    ASSERT_EQ(file.size(), bytes.size());
    EXPECT_EQ(std::vector<std::uint8_t>(file.data(), file.data() + file.size()), bytes);
  }
  std::filesystem::remove(path);
}

TEST(MappedFile, EmptyFileHasNoBytes) {
  const auto path = write_bytes("itchbook-mapped-file-empty", {});
  {
    const itchbook::MappedFile file(path);
    EXPECT_EQ(file.size(), std::size_t{0});
    EXPECT_EQ(file.data(), nullptr);
  }
  std::filesystem::remove(path);
}

TEST(MappedFile, MissingFileThrows) {
  EXPECT_THROW(itchbook::MappedFile("itchbook-missing-file"), std::filesystem::filesystem_error);
}

// Checked by the compiler, not at run time: copying must not compile, moving
// must, and moving must be noexcept so std::vector<MappedFile> can grow by moving.
static_assert(!std::is_copy_constructible_v<itchbook::MappedFile>);
static_assert(!std::is_copy_assignable_v<itchbook::MappedFile>);
static_assert(std::is_nothrow_move_constructible_v<itchbook::MappedFile>);
static_assert(std::is_nothrow_move_assignable_v<itchbook::MappedFile>);

TEST(MappedFile, MoveConstructHandsOverTheMapping) {
  const auto path = write_bytes("itchbook-mapped-file-move", {0x00, 0x01, 'Z'});
  {
    itchbook::MappedFile first(path);
    const std::uint8_t* bytes = first.data();

    itchbook::MappedFile second(std::move(first));
    EXPECT_EQ(second.data(), bytes);  // the same mapping, not a copy of it
    EXPECT_EQ(second.size(), 3u);
    EXPECT_EQ(second.data()[2], 'Z');
    EXPECT_EQ(first.data(), nullptr);  // moved-from: empty, and its destructor does nothing
    EXPECT_EQ(first.size(), 0u);
  }  // only second unmaps; ASan would report it if both did
  std::filesystem::remove(path);
}

TEST(MappedFile, MoveAssignReleasesTheOldMapping) {
  const auto path_a = write_bytes("itchbook-mapped-file-a", {'A', 'A'});
  const auto path_b = write_bytes("itchbook-mapped-file-b", {'B', 'B', 'B'});
  {
    itchbook::MappedFile a(path_a);
    itchbook::MappedFile b(path_b);

    a = std::move(b);  // a's old mapping is released, then a takes b's
    EXPECT_EQ(a.size(), 3u);
    EXPECT_EQ(a.data()[0], 'B');
    EXPECT_EQ(b.data(), nullptr);

    auto& same = a;  // through a reference, so the compiler does not warn about self-move
    a = std::move(same);
    EXPECT_EQ(a.size(), 3u);  // self-move left it as it was
    EXPECT_EQ(a.data()[0], 'B');
  }
  std::filesystem::remove(path_a);
  std::filesystem::remove(path_b);
}

TEST(FlatMap, InsertFindErase) {
  itchbook::FlatMap<std::uint64_t, int> map;
  EXPECT_TRUE(map.empty());
  EXPECT_TRUE(map.insert(42, 1));
  EXPECT_FALSE(map.insert(42, 2));  // already there: unchanged, like emplace
  ASSERT_NE(map.find(42), nullptr);
  EXPECT_EQ(*map.find(42), 1);
  EXPECT_EQ(map.find(7), nullptr);

  *map.find(42) = 5;  // find gives a pointer to the stored value
  EXPECT_EQ(*map.find(42), 5);

  EXPECT_TRUE(map.erase(42));
  EXPECT_FALSE(map.erase(42));
  EXPECT_EQ(map.find(42), nullptr);
  EXPECT_EQ(map.size(), 0u);
}

TEST(FlatMap, GrowsAndStaysAtMostHalfFull) {
  itchbook::FlatMap<std::uint64_t, std::uint64_t> map;
  for (std::uint64_t k = 1; k <= 100'000; ++k) {
    ASSERT_TRUE(map.insert(k, k * 3));
    ASSERT_LE(map.size() * 2, map.capacity());
  }
  for (std::uint64_t k = 1; k <= 100'000; ++k) {
    ASSERT_NE(map.find(k), nullptr) << k;
    ASSERT_EQ(*map.find(k), k * 3);
  }
  EXPECT_EQ(std::popcount(map.capacity()), 1);  // always a power of two
}

TEST(FlatMap, ReserveAvoidsGrowing) {
  itchbook::FlatMap<std::uint64_t, int> map;
  map.reserve(1'000);
  const std::size_t capacity = map.capacity();
  EXPECT_GE(capacity, 2'000u);
  for (std::uint64_t k = 0; k < 1'000; ++k) {
    map.insert(k, 0);
  }
  EXPECT_EQ(map.capacity(), capacity);
}

TEST(FlatMap, EraseKeepsCollidingKeysReachable) {
  // A tiny table that never grows, so keys collide and runs wrap past the end
  // of the array. Erasing from the middle of a run must not hide anything after it.
  itchbook::FlatMap<std::uint64_t, std::uint64_t> map;  // 16 slots
  std::vector<std::uint64_t> keys;
  // Keys whose home is slot 15, the last one (the top 4 bits of the hash, as
  // FlatMap computes it for 16 slots), so their run wraps around to slot 0.
  for (std::uint64_t k = 1; keys.size() < 8; ++k) {
    if ((k * 0x9E3779B97F4A7C15ull) >> 60 == 15) {
      keys.push_back(k);
      map.insert(k, k);
    }
  }
  ASSERT_EQ(map.capacity(), 16u);
  for (std::size_t i = 0; i < keys.size(); i += 2) {
    ASSERT_TRUE(map.erase(keys[i]));
    for (std::size_t j = 0; j < keys.size(); ++j) {
      const bool erased = j % 2 == 0 && j <= i;
      EXPECT_EQ(map.find(keys[j]) == nullptr, erased) << "key " << keys[j] << " after erasing "
                                                      << keys[i];
    }
  }
}

// The differential test: the same 10 million random operations on FlatMap and
// on std::unordered_map, which we trust. Every answer must match. Keys come
// from a small range, so the table keeps filling, growing, colliding and
// emptying.
TEST(FlatMap, MatchesUnorderedMapOnTenMillionRandomOps) {
  itchbook::FlatMap<std::uint64_t, std::uint64_t> flat;
  std::unordered_map<std::uint64_t, std::uint64_t> reference;
  std::mt19937_64 rng(2019'12'30);
  std::uniform_int_distribution<std::uint64_t> any_key(0, (1u << 18) - 1);
  std::uniform_int_distribution<int> any_op(0, 99);

  for (int step = 0; step < 10'000'000; ++step) {
    const std::uint64_t key = any_key(rng);
    const int op = any_op(rng);
    if (op < 40) {
      const std::uint64_t value = rng();
      const bool want = reference.emplace(key, value).second;
      ASSERT_EQ(flat.insert(key, value), want) << "insert " << key << " at step " << step;
    } else if (op < 75) {
      const bool want = reference.erase(key) == 1;
      ASSERT_EQ(flat.erase(key), want) << "erase " << key << " at step " << step;
    } else {
      const auto it = reference.find(key);
      const std::uint64_t* got = flat.find(key);
      ASSERT_EQ(got != nullptr, it != reference.end()) << "find " << key << " at step " << step;
      if (got != nullptr) {
        ASSERT_EQ(*got, it->second) << "value of " << key << " at step " << step;
      }
    }
    ASSERT_EQ(flat.size(), reference.size()) << "at step " << step;
  }
  // Finally every surviving key, both ways.
  for (const auto& [key, value] : reference) {
    ASSERT_NE(flat.find(key), nullptr);
    ASSERT_EQ(*flat.find(key), value);
  }
}

namespace {

// A BookV2 with a bug planted on purpose: it ignores cancels.
class BookIgnoringCancels : public itchbook::BookV2 {
 public:
  void on_cancel(const itchbook::OrderCancel&) override {}
};

// All on stock 13, the locate the execute/cancel/remove/replace helpers put
// in their headers. DiffBooks compares the stock a message's header names,
// which in real ITCH is always the stock of the order it touches.
std::vector<std::vector<std::uint8_t>> busy_script() {
  return {add(1, 'B', 100, 10'000, 13), add(2, 'B', 50, 10'000, 13),
          add(3, 'S', 200, 10'100, 13), execute(1, 30),
          replace(3, 4, 80, 10'050),    cancel(2, 20),
          remove(1),                    execute_with_price(4, 80, 10'060)};
}

}  // namespace

TEST(DiffBooks, AgreeingBooksHaveNoMismatches) {
  itchbook::DiffBooks<itchbook::BookV1, itchbook::BookV2> diff;
  for (const auto& m : busy_script()) {
    feed(diff, m);
  }
  EXPECT_EQ(diff.compared(), 8u);
  EXPECT_EQ(diff.mismatches(), 0u);
  EXPECT_FALSE(diff.first_mismatch().has_value());
}

TEST(DiffBooks, CatchesABrokenBook) {
  itchbook::DiffBooks<itchbook::BookV1, BookIgnoringCancels> diff;
  for (const auto& m : busy_script()) {
    feed(diff, m);
  }
  EXPECT_GT(diff.mismatches(), 0u);
  ASSERT_TRUE(diff.first_mismatch().has_value());
  EXPECT_EQ(diff.first_mismatch()->type, 'X');  // the first cancel is where they part
}

static_assert(!std::is_copy_constructible_v<itchbook::DirectOrderStore>);
static_assert(std::is_nothrow_move_constructible_v<itchbook::DirectOrderStore>);

TEST(DirectOrderStore, IndexesByReference) {
  itchbook::DirectOrderStore store;
  EXPECT_EQ(store.find(5), nullptr);  // nothing allocated yet
  EXPECT_TRUE(store.insert(5, {1, itchbook::Side::Buy, 100, 10}));
  EXPECT_FALSE(store.insert(5, {1, itchbook::Side::Buy, 100, 10}));  // already live
  ASSERT_NE(store.find(5), nullptr);
  EXPECT_EQ(store.find(5)->shares, 10u);
  EXPECT_EQ(store.find(6), nullptr);
  EXPECT_EQ(store.size(), 1u);

  // Past the day's largest reference: the array grows and keeps what it had.
  const std::uint64_t far = itchbook::day_largest_order_ref + 10;
  EXPECT_TRUE(store.insert(far, {2, itchbook::Side::Sell, 200, 3}));
  EXPECT_EQ(store.find(5)->shares, 10u);
  EXPECT_EQ(store.find(far)->price, 200u);

  EXPECT_TRUE(store.erase(5));
  EXPECT_FALSE(store.erase(5));
  EXPECT_EQ(store.find(5), nullptr);
  EXPECT_EQ(store.size(), 1u);
}
