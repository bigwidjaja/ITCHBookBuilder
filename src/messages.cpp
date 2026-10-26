#include "itchbook/messages.hpp"

#include "itchbook/core.hpp"
#include "itchbook/mapped_file.hpp"

#include <algorithm>
#include <cstddef>

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

// The virtual version: dispatch with H = Handler, so each call goes through
// the vtable. One switch serves both paths.
bool parse_message(const std::uint8_t* p, std::size_t length, Handler& h) {
  return dispatch(p, length, h);
}

MessageReader::MessageReader(std::span<const std::uint8_t> bytes, std::uint64_t offset)
    : bytes_(bytes),
      offset_(std::min<std::uint64_t>(offset, bytes.size())),
      next_(offset_) {}

bool MessageReader::next() {
  // The same walk as Step 1: a 2-byte length, then that many bytes. The
  // checks subtract from the size instead of adding to next_, so they cannot
  // overflow, and next_ never passes the end.
  const std::uint64_t left = bytes_.size() - next_;
  if (left < 2) {
    return false;
  }
  const std::uint16_t length = load_be16(bytes_.data() + next_);
  if (left - 2 < length) {
    return false;  // the file ends partway through this message
  }
  offset_ = next_;
  data_ = bytes_.data() + next_ + 2;
  size_ = length;
  next_ += 2 + length;
  return true;
}

void parse_file(const std::filesystem::path& path, Handler& h) {
  const MappedFile file(path);
  MessageReader reader(file.bytes());
  while (!h.done() && reader.next()) {
    parse_message(reader.data(), reader.size(), h);
  }
}

}  // namespace itchbook
