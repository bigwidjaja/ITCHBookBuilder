#include "itchbook/core.hpp"

#include "itchbook/mapped_file.hpp"
#include "itchbook/messages.hpp"

#include <cstdint>
#include <system_error>

namespace itchbook {

std::uintmax_t file_size(const std::filesystem::path& path) {
  return std::filesystem::file_size(path);
}

std::ifstream open_binary(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    throw std::filesystem::filesystem_error(
        "failed to open file", path, std::make_error_code(std::errc::io_error));
  }
  return f;
}

/*
read_messages walks a NASDAQ ITCH sample file. On disk, each exchange message is preceded by a 2-byte length, because an add, a delete, and an execute are different sizes. The function pulls those messages out one by one.

1. Open the ITCH file in binary mode. The bytes must stay exactly as the exchange wrote them.
2. Prepare an empty list, messages, and one reusable buffer, buf. b holds the 2-byte length that sits in front of each message.
3. Read those 2 bytes into b. If the read fails, the file ended on a message boundary, so the walk stops.
4. Turn the two bytes into length. Bytes 0x00 0x03 mean the next message is 3 bytes long. The first byte is the high one, so 0x01 0x00 means 256.
5. Resize buf to that many bytes.
6. Read the message itself into buf. The first byte of that message is its type, such as A for add or D for delete. If the file is cut off in the middle of the message, which can happen in data/small.itch, skip that partial message and stop.
7. Copy the finished message onto messages. The 2-byte length prefix is not part of the saved message.
8. Go back to step 3. The next length prefix says how big the next message is, and buf is resized to fit it.
9. Return every complete message. The later count step will look at the first byte of each one and tally the types.
*/

// <> picks what a vector holds. Inner vector<uint8_t> is one message, and outer vector<...> is the list of those messages.
std::vector<std::vector<std::uint8_t>> read_messages(const std::filesystem::path& path) {
  std::ifstream f = open_binary(path);
  std::vector<std::vector<std::uint8_t>> messages;
  std::vector<std::uint8_t> buf;
  std::uint8_t b[2];  // b[0] and b[1]. The file stores each length in exactly 2 bytes cuz our msgs are 2 bytes long.

  // The while check is this read: keep going only when both length bytes arrived.
  // read() writes through char*, so the cast points it at b. 2 is how many bytes.
  while (f.read(reinterpret_cast<char*>(b), 2)) {  // reinterpret_cast<char*>(b) means “this address is a char* now,” which lets read store bytes into b[0] and b[1]. f.read only accepts a char*, b is a uint8_t* prev
    // These two lines run after the read succeeds. b[0] is the high byte: << 8 then | b[1].
    const std::uint16_t length = static_cast<std::uint16_t>((b[0] << 8) | b[1]); // b[0] and b[1] shift 8 to the left, multiplying it by 256
    // That line turns the two length bytes into the number of bytes in the next message. The loop then uses length to size buf and to read exactly that many bytes from the file.
    buf.resize(length);  // grow buf to `length` bytes so the next read can fill the message
    if (length > 0 &&
        !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(length))) {
      break;
    }
    messages.push_back(buf);
  }
  return messages;
}

MessageCounts count_messages(const std::filesystem::path& path) {
  // 256 slots, one per byte value 0..255. {} sets every count to 0.
  // 'A' is byte 65, so an add order increments by_type[65].
  MessageCounts result;
  // Since Step 9 this walks the mapped file instead of copying each message
  // out of an ifstream. read_messages above keeps the Step 1 ifstream loop.
  const MappedFile file(path);
  MessageReader reader(file.bytes());
  while (reader.next()) {
    if (reader.size() == 0) {
      continue;
    }
    const std::uint8_t type = reader.data()[0];
    result.by_type[type]++;
    ++result.messages;
    const std::uint16_t expected = expected_lengths[type];
    if (expected != 0 && expected != reader.size()) {
      ++result.mismatches;
    }
  }
  result.truncated = reader.truncated();
  return result;
}

}  // namespace itchbook
