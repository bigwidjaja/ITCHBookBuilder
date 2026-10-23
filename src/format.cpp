#include "itchbook/format.hpp"

#include <charconv>
#include <format>
#include <system_error>

namespace itchbook {

namespace {

// Parses text as exactly `digits` decimal digits.
std::optional<std::uint64_t> parse_digits(std::string_view text, std::size_t digits) {
  if (text.size() != digits) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

std::string format_timestamp(std::uint64_t ns) {
  constexpr std::uint64_t ns_per_second = 1'000'000'000;
  const std::uint64_t total_seconds = ns / ns_per_second;
  const std::uint64_t hours = total_seconds / 3600;
  const std::uint64_t minutes = total_seconds / 60 % 60;
  const std::uint64_t seconds = total_seconds % 60;
  const std::uint64_t nanos = ns % ns_per_second;
  // {:02} pads to 2 digits with zeros, {:09} to 9.
  return std::format("{:02}:{:02}:{:02}.{:09}", hours, minutes, seconds, nanos);
}

std::string format_price(std::uint32_t price) {
  // The last 4 digits are the fraction: 1234500 is 123 dollars and 4500/10000.
  return std::format("{}.{:04}", price / 10'000, price % 10'000);
}

std::optional<std::uint64_t> parse_timestamp(std::string_view text) {
  // HH:MM:SS is 8 characters; a fraction adds '.' and 1 to 9 digits.
  if (text.size() < 8 || text[2] != ':' || text[5] != ':') {
    return std::nullopt;
  }
  const auto hours = parse_digits(text.substr(0, 2), 2);
  const auto minutes = parse_digits(text.substr(3, 2), 2);
  const auto seconds = parse_digits(text.substr(6, 2), 2);
  if (!hours || !minutes || !seconds || *hours > 23 || *minutes > 59 || *seconds > 59) {
    return std::nullopt;
  }
  std::uint64_t nanos = 0;
  if (text.size() > 8) {
    const std::string_view fraction = text.substr(9);
    if (text[8] != '.' || fraction.empty() || fraction.size() > 9) {
      return std::nullopt;
    }
    const auto value = parse_digits(fraction, fraction.size());
    if (!value) {
      return std::nullopt;
    }
    // ".5" is 500000000 ns: pad the digits out to 9 places.
    nanos = *value;
    for (std::size_t i = fraction.size(); i < 9; ++i) {
      nanos *= 10;
    }
  }
  return ((*hours * 60 + *minutes) * 60 + *seconds) * 1'000'000'000 + nanos;
}

}  // namespace itchbook
