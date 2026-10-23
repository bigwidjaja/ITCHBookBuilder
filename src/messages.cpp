#include "itchbook/messages.hpp"

#include "itchbook/core.hpp"

#include <cstddef>
#include <fstream>
#include <vector>

namespace itchbook {

namespace {

// Views n bytes as text. Casting to const char* is the allowed direction:
// any bytes may be looked at as characters.
std::string_view text(const std::uint8_t* p, std::size_t n) {
  return {reinterpret_cast<const char*>(p), n};
}

}  // namespace

// Each decoder reads its fields at the offsets in messages.hpp. Numbers go
// through load_be*, one-byte fields are read directly, and text fields become
// string_views into the message. Stock symbols have their space padding trimmed.

MessageHeader decode_header(const std::uint8_t* p) {
  return {
      .type = static_cast<char>(p[0]),
      .stock_locate = load_be16(p + 1),
      .tracking_number = load_be16(p + 3),
      .timestamp = load_be48(p + 5),
  };
}

SystemEvent decode_system_event(const std::uint8_t* p) {
  return {
      .header = decode_header(p),
      .event_code = static_cast<char>(p[11]),
  };
}

StockDirectory decode_stock_directory(const std::uint8_t* p) {
  return {
      .header = decode_header(p),
      .stock = trim_symbol(text(p + 11, 8)),
      .market_category = static_cast<char>(p[19]),
      .financial_status = static_cast<char>(p[20]),
      .round_lot_size = load_be32(p + 21),
  };
}

AddOrder decode_add_order(const std::uint8_t* p) {
  return {
      .header = decode_header(p),
      .order_ref = load_be64(p + 11),
      .side = static_cast<Side>(p[19]),
      .shares = load_be32(p + 20),
      .stock = trim_symbol(text(p + 24, 8)),
      .price = load_be32(p + 32),
      .attribution = p[0] == 'F' ? text(p + 36, 4) : std::string_view{},
  };
}

OrderExecuted decode_order_executed(const std::uint8_t* p) {
  return {
      .header = decode_header(p),
      .order_ref = load_be64(p + 11),
      .executed_shares = load_be32(p + 19),
      .match_number = load_be64(p + 23),
  };
}

OrderExecutedWithPrice decode_order_executed_with_price(const std::uint8_t* p) {
  return {
      .header = decode_header(p),
      .order_ref = load_be64(p + 11),
      .executed_shares = load_be32(p + 19),
      .match_number = load_be64(p + 23),
      .printable = p[31] == 'Y',
      .execution_price = load_be32(p + 32),
  };
}

OrderCancel decode_order_cancel(const std::uint8_t* p) {
  return {
      .header = decode_header(p),
      .order_ref = load_be64(p + 11),
      .cancelled_shares = load_be32(p + 19),
  };
}

OrderDelete decode_order_delete(const std::uint8_t* p) {
  return {
      .header = decode_header(p),
      .order_ref = load_be64(p + 11),
  };
}

OrderReplace decode_order_replace(const std::uint8_t* p) {
  return {
      .header = decode_header(p),
      .original_order_ref = load_be64(p + 11),
      .new_order_ref = load_be64(p + 19),
      .shares = load_be32(p + 27),
      .price = load_be32(p + 31),
  };
}

bool parse_message(const std::uint8_t* p, std::size_t length, Handler& h) {
  // Types we do not decode have expected length 0, so this also rejects them.
  if (length == 0 || length != expected_lengths[p[0]]) {
    return false;
  }
  // Every spec message is at least 12 bytes, so the 11-byte header is there.
  // The decoders below read the header again; that is fine for now.
  h.on_message(decode_header(p));
  switch (p[0]) {
    case 'S':
      h.on_system_event(decode_system_event(p));
      return true;
    case 'R':
      h.on_stock_directory(decode_stock_directory(p));
      return true;
    case 'A':
    case 'F':
      h.on_add(decode_add_order(p));
      return true;
    case 'E':
      h.on_execute(decode_order_executed(p));
      return true;
    case 'C':
      h.on_execute_with_price(decode_order_executed_with_price(p));
      return true;
    case 'X':
      h.on_cancel(decode_order_cancel(p));
      return true;
    case 'D':
      h.on_delete(decode_order_delete(p));
      return true;
    case 'U':
      h.on_replace(decode_order_replace(p));
      return true;
    default:
      return false;
  }
}

void parse_file(const std::filesystem::path& path, Handler& h) {
  // The same walk as count_messages: 2-byte length, then that many bytes.
  std::ifstream f = open_binary(path);
  std::vector<std::uint8_t> buf;
  std::uint8_t b[2];

  while (!h.done() && f.read(reinterpret_cast<char*>(b), 2)) {
    const std::uint16_t length = load_be16(b);
    buf.resize(length);
    if (length > 0 &&
        !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(length))) {
      break;
    }
    parse_message(buf.data(), length, h);
  }
}

}  // namespace itchbook
