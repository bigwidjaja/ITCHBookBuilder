#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace itchbook {

// Formats nanoseconds since midnight as HH:MM:SS.nnnnnnnnn.
// 34200000000000 becomes "09:30:00.000000000".
std::string format_timestamp(std::uint64_t ns);

// Formats an ITCH price (4 implied decimals) as dollars with 4 decimals.
// 1234500 becomes "123.4500". Uses integer math only, so no rounding.
std::string format_price(std::uint32_t price);

// The reverse of format_timestamp: parses HH:MM:SS with an optional fraction
// of up to 9 digits into nanoseconds since midnight. "10:30:00" becomes
// 37800000000000 and "10:30:00.5" adds 500000000. Returns nullopt for
// anything else, including out-of-range fields like 24:00:00.
std::optional<std::uint64_t> parse_timestamp(std::string_view text);

}  // namespace itchbook
