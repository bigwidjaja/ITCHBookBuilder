#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace itchbook {

// Big-endian loads. Each reads bytes at p, most significant byte first, and
// builds the value with shifts. Every byte is widened to the result type
// before shifting, so a byte >= 0x80 never overflows a signed int.
constexpr std::uint16_t load_be16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((std::uint16_t{p[0]} << 8) | std::uint16_t{p[1]});
}

constexpr std::uint32_t load_be32(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
         (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

// 6 bytes, used for timestamps. C++ has no 48-bit integer, so the result is
// a uint64_t with the top two bytes zero.
constexpr std::uint64_t load_be48(const std::uint8_t* p) {
  return (std::uint64_t{p[0]} << 40) | (std::uint64_t{p[1]} << 32) |
         (std::uint64_t{p[2]} << 24) | (std::uint64_t{p[3]} << 16) |
         (std::uint64_t{p[4]} << 8) | std::uint64_t{p[5]};
}

constexpr std::uint64_t load_be64(const std::uint8_t* p) {
  return (std::uint64_t{load_be32(p)} << 32) | load_be32(p + 4);
}

// Returns the size in bytes. Throws std::filesystem::filesystem_error if path
// cannot be read.
std::uintmax_t file_size(const std::filesystem::path& path);

// Opens path for binary reading. Throws std::filesystem::filesystem_error if
// the file cannot be opened.
std::ifstream open_binary(const std::filesystem::path& path);

// Walks length-prefixed messages. Each message is a 2-byte big-endian length
// followed by that many bytes. A message cut off at the end of the file is
// left out. Returns each complete message, without its length prefix.
std::vector<std::vector<std::uint8_t>> read_messages(const std::filesystem::path& path);

// Spec length for each message type. Slot 0 means the type is not in the spec.
// 'h' and 'H' are different messages.
constexpr std::array<std::uint16_t, 256> expected_message_lengths() {
  std::array<std::uint16_t, 256> lengths{};
  lengths['S'] = 12;
  lengths['R'] = 39;
  lengths['H'] = 25;
  lengths['Y'] = 20;
  lengths['L'] = 26;
  lengths['V'] = 35;
  lengths['W'] = 12;
  lengths['K'] = 28;
  lengths['J'] = 35;
  lengths['h'] = 21;
  lengths['A'] = 36;
  lengths['F'] = 40;
  lengths['E'] = 31;
  lengths['C'] = 36;
  lengths['X'] = 23;
  lengths['D'] = 19;
  lengths['U'] = 35;
  lengths['P'] = 44;
  lengths['Q'] = 40;
  lengths['B'] = 19;
  lengths['I'] = 50;
  lengths['N'] = 20;
  lengths['O'] = 48;
  return lengths;
}

inline constexpr std::array<std::uint16_t, 256> expected_lengths = expected_message_lengths();

struct MessageCounts {
  std::array<std::uint64_t, 256> by_type{};
  std::uint64_t messages = 0;
  std::uint64_t mismatches = 0;
  bool truncated = false;
};

// Counts messages by type. A spec type whose length differs from expected_lengths
// increments mismatches. A file that ends mid-message sets truncated and keeps
// the complete messages already read.
MessageCounts count_messages(const std::filesystem::path& path);

}  // namespace itchbook
