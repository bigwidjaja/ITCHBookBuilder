#include "itchbook/book_v1.hpp"
#include "itchbook/core.hpp"
#include "itchbook/dump.hpp"
#include "itchbook/format.hpp"
#include "itchbook/messages.hpp"
#include "itchbook/replay.hpp"
#include "itchbook/stats.hpp"
#include "itchbook/symbols.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

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

TEST(BookV1, AddBuildsLevelsBestFirst) {
  itchbook::BookV1 book;
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

TEST(BookV1, TopLimitsDepth) {
  itchbook::BookV1 book;
  feed(book, add(1, 'B', 10, 100));
  feed(book, add(2, 'B', 10, 99));
  feed(book, add(3, 'B', 10, 98));

  EXPECT_EQ(book.top(1, 2).bids, (Levels{{100, 10}, {99, 10}}));
}

TEST(BookV1, ExecuteReducesThenRemoves) {
  itchbook::BookV1 book;
  feed(book, add(1, 'B', 10, 100));

  feed(book, execute(1, 4));
  EXPECT_EQ(book.top(1, 5).bids, (Levels{{100, 6}}));
  EXPECT_EQ(book.order_count(), 1u);

  feed(book, execute(1, 6));  // the rest: order and level both go
  EXPECT_TRUE(book.top(1, 5).bids.empty());
  EXPECT_EQ(book.order_count(), 0u);
}

TEST(BookV1, ExecuteWithPriceUsesOrderPrice) {
  itchbook::BookV1 book;
  feed(book, add(1, 'S', 10, 100));
  feed(book, execute_with_price(1, 4, 105));  // traded at 105, but rests at 100

  EXPECT_EQ(book.top(1, 5).asks, (Levels{{100, 6}}));
}

TEST(BookV1, CancelReducesShares) {
  itchbook::BookV1 book;
  feed(book, add(1, 'B', 10, 100));
  feed(book, cancel(1, 3));

  EXPECT_EQ(book.top(1, 5).bids, (Levels{{100, 7}}));
  EXPECT_EQ(book.order_count(), 1u);
}

TEST(BookV1, DeleteRemovesOnlyThatOrder) {
  itchbook::BookV1 book;
  feed(book, add(1, 'B', 10, 100));
  feed(book, add(2, 'B', 5, 100));
  feed(book, remove(1));

  EXPECT_EQ(book.top(1, 5).bids, (Levels{{100, 5}}));
  EXPECT_EQ(book.order_count(), 1u);
}

TEST(BookV1, ReplaceKeepsLocateAndSide) {
  itchbook::BookV1 book;
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

TEST(BookV1, BooksAreSeparatePerStock) {
  itchbook::BookV1 book;
  feed(book, add(1, 'B', 10, 100, 1));
  feed(book, add(2, 'B', 20, 200, 2));

  EXPECT_EQ(book.top(1, 5).bids, (Levels{{100, 10}}));
  EXPECT_EQ(book.top(2, 5).bids, (Levels{{200, 20}}));
  EXPECT_TRUE(book.top(3, 5).bids.empty());  // never seen
}

TEST(BookV1, UnknownOrDuplicateRefIsAnError) {
  itchbook::BookV1 book;
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
TEST(BookV1DeathTest, ExecuteMoreThanOrderAsserts) {
  itchbook::BookV1 book;
  feed(book, add(1, 'B', 10, 100));
  EXPECT_DEBUG_DEATH(feed(book, execute(1, 11)), "shares <= order.shares");
}

TEST(BookV1DeathTest, CancelMoreThanOrderAsserts) {
  itchbook::BookV1 book;
  feed(book, add(1, 'S', 10, 100));
  EXPECT_DEBUG_DEATH(feed(book, cancel(1, 11)), "shares <= order.shares");
}

namespace {

// One step of a scripted sequence: a message, then both sides of locate 1's
// book as they should look after it.
struct ScriptStep {
  std::vector<std::uint8_t> message;
  Levels bids;
  Levels asks;
};

void run_script(const std::vector<ScriptStep>& script, std::uint16_t locate = 1) {
  itchbook::BookV1 book;
  for (std::size_t i = 0; i < script.size(); ++i) {
    feed(book, script[i].message);
    const auto top = book.top(locate, 10);
    EXPECT_EQ(top.bids, script[i].bids) << "bids after step " << i;
    EXPECT_EQ(top.asks, script[i].asks) << "asks after step " << i;
  }
  EXPECT_EQ(book.errors(), 0u);
}

}  // namespace

TEST(BookV1Script, AddPartialExecuteReplaceDelete) {
  run_script({
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

TEST(BookV1Script, ReplaceToSameLevelAndBack) {
  run_script({
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
TEST(BookV1Script, ArgxReplaceFromRealData) {
  run_script(
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

TEST(BookV1, PeakOrderCount) {
  itchbook::BookV1 book;
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

TEST(BookV1, State) {
  itchbook::BookV1 book;
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

TEST(Core, ReadMessagesStopsWhenCutOff) {
  const auto path = write_bytes("itchbook-read-messages-cutoff", {0x00, 0x04, 'A', 'B'});
  const auto messages = itchbook::read_messages(path);
  std::filesystem::remove(path);

  EXPECT_TRUE(messages.empty());
}
