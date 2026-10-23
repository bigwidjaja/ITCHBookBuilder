#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace itchbook {

// Decoded ITCH 5.0 messages. These are not the wire layout: the compiler may
// pad these structs, and every number here is already in the CPU's byte order.
// Comments give each field's offset and size in the raw message.
//
// Prices are integers with 4 implied decimals: 1234500 means $123.4500.
// string_view fields point into the message buffer and are only valid until
// that buffer is reused.

enum class Side : char { Buy = 'B', Sell = 'S' };

// The first 11 bytes of every message. type is stored because one struct can
// hold more than one type: AddOrder is either 'A' or 'F'.
struct MessageHeader {
  char type;                      // offset 0, 1 byte
  std::uint16_t stock_locate;     // offset 1, 2 bytes
  std::uint16_t tracking_number;  // offset 3, 2 bytes
  std::uint64_t timestamp;        // offset 5, 6 bytes, nanoseconds since midnight
};

// 'S', 12 bytes.
struct SystemEvent {
  MessageHeader header;
  char event_code;  // offset 11: O start of messages, S start of system hours,
                    // Q start of market hours, M end of market hours,
                    // E end of system hours, C end of messages
};

// 'R', 39 bytes. Fields after round_lot_size are skipped until something needs them.
struct StockDirectory {
  MessageHeader header;
  std::string_view stock;        // offset 11, 8 bytes, padding trimmed
  char market_category;          // offset 19, 1 byte
  char financial_status;         // offset 20, 1 byte
  std::uint32_t round_lot_size;  // offset 21, 4 bytes
};

// 'A' (36 bytes) and 'F' (40 bytes). The book treats them the same; F adds
// the market participant id, which is empty for A.
struct AddOrder {
  MessageHeader header;
  std::uint64_t order_ref;      // offset 11, 8 bytes
  Side side;                    // offset 19, 1 byte
  std::uint32_t shares;         // offset 20, 4 bytes
  std::string_view stock;       // offset 24, 8 bytes, padding trimmed
  std::uint32_t price;          // offset 32, 4 bytes
  std::string_view attribution; // offset 36, 4 bytes, F only
};

// 'E', 31 bytes. Part or all of a resting order traded at its own price.
struct OrderExecuted {
  MessageHeader header;
  std::uint64_t order_ref;        // offset 11, 8 bytes
  std::uint32_t executed_shares;  // offset 19, 4 bytes
  std::uint64_t match_number;     // offset 23, 8 bytes
};

// 'C', 36 bytes. Like E, but traded at a different price than the order's.
struct OrderExecutedWithPrice {
  MessageHeader header;
  std::uint64_t order_ref;        // offset 11, 8 bytes
  std::uint32_t executed_shares;  // offset 19, 4 bytes
  std::uint64_t match_number;     // offset 23, 8 bytes
  bool printable;                 // offset 31, 1 byte, 'Y' or 'N'
  std::uint32_t execution_price;  // offset 32, 4 bytes
};

// 'X', 23 bytes. Removes some shares; the order stays in the book.
struct OrderCancel {
  MessageHeader header;
  std::uint64_t order_ref;         // offset 11, 8 bytes
  std::uint32_t cancelled_shares;  // offset 19, 4 bytes
};

// 'D', 19 bytes. Removes the whole order.
struct OrderDelete {
  MessageHeader header;
  std::uint64_t order_ref;  // offset 11, 8 bytes
};

// 'U', 35 bytes. Deletes the original order and adds a new one with the same
// side and stock, under a new reference number.
struct OrderReplace {
  MessageHeader header;
  std::uint64_t original_order_ref;  // offset 11, 8 bytes
  std::uint64_t new_order_ref;       // offset 19, 8 bytes
  std::uint32_t shares;              // offset 27, 4 bytes
  std::uint32_t price;               // offset 31, 4 bytes
};

// Drops the trailing space padding from a symbol field: "AAPL    " becomes
// "AAPL". The result still points into the same bytes; nothing is copied.
constexpr std::string_view trim_symbol(std::string_view s) {
  const std::size_t last = s.find_last_not_of(' ');
  return last == std::string_view::npos ? std::string_view{} : s.substr(0, last + 1);
}

// Decoders. Each takes p pointing at the type byte of one complete message of
// that type, so p[0] is the type and the message is the length in the spec.
// The caller checks the length first; these do not.
MessageHeader decode_header(const std::uint8_t* p);
SystemEvent decode_system_event(const std::uint8_t* p);
StockDirectory decode_stock_directory(const std::uint8_t* p);
AddOrder decode_add_order(const std::uint8_t* p);  // 'A' or 'F'
OrderExecuted decode_order_executed(const std::uint8_t* p);
OrderExecutedWithPrice decode_order_executed_with_price(const std::uint8_t* p);
OrderCancel decode_order_cancel(const std::uint8_t* p);
OrderDelete decode_order_delete(const std::uint8_t* p);
OrderReplace decode_order_replace(const std::uint8_t* p);

// Receives decoded messages from parse_message. Each function does nothing by
// default, so a handler overrides only the messages it cares about.
// Virtual calls are simple but not free; Step 12 revisits this.
class Handler {
 public:
  virtual ~Handler() = default;

  // Called first for every message with a valid length, including types
  // without their own function below.
  virtual void on_message(const MessageHeader&) {}

  virtual void on_system_event(const SystemEvent&) {}
  virtual void on_stock_directory(const StockDirectory&) {}
  virtual void on_add(const AddOrder&) {}  // 'A' and 'F'
  virtual void on_execute(const OrderExecuted&) {}
  virtual void on_execute_with_price(const OrderExecutedWithPrice&) {}
  virtual void on_cancel(const OrderCancel&) {}
  virtual void on_delete(const OrderDelete&) {}
  virtual void on_replace(const OrderReplace&) {}

  // parse_file checks this after each message and stops once it is true.
  virtual bool done() const { return false; }
};

// Decodes one message and passes it to h. p points at the type byte and
// length is the message length from its 2-byte prefix.
// If length matches the spec, calls h.on_message with the header, then the
// type's own function if it is one we decode. Returns true if that second
// call happened. A length that does not match the spec calls nothing.
bool parse_message(const std::uint8_t* p, std::size_t length, Handler& h);

// Reads length-prefixed messages one at a time, starting at any message
// boundary. offset() says where the current message starts, so a later
// reader can resume at exactly that message.
class MessageReader {
 public:
  // Throws std::filesystem::filesystem_error if path cannot be opened.
  explicit MessageReader(const std::filesystem::path& path, std::uint64_t offset = 0);

  // Reads the next complete message. Returns false at the end of the file or
  // at a message cut off by it.
  bool next();

  // The current message, without its length prefix: data()[0] is the type.
  const std::uint8_t* data() const { return buf_.data(); }
  std::size_t size() const { return buf_.size(); }

  // Byte offset of the current message's 2-byte length prefix.
  std::uint64_t offset() const { return offset_; }

 private:
  std::ifstream f_;
  std::vector<std::uint8_t> buf_;
  std::uint64_t offset_ = 0;  // the current message
  std::uint64_t next_ = 0;    // the message after it
};

// Walks every message in an ITCH file and passes each one to parse_message.
// Stops at the end of the file, at a message cut off by the end of the file,
// or once h.done() is true. Throws std::filesystem::filesystem_error if path
// cannot be opened.
void parse_file(const std::filesystem::path& path, Handler& h);

}  // namespace itchbook
