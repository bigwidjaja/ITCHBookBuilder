#include "itchbook/symbols.hpp"

namespace itchbook {

void SymbolDirectory::add(std::uint16_t locate, std::string_view symbol) {
  // Grow so that symbols_[locate] exists. A uint16_t locate caps this at
  // 65536 slots.
  if (locate >= symbols_.size()) {
    symbols_.resize(std::size_t{locate} + 1);
  }
  if (symbols_[locate].empty()) {
    ++count_;
  }
  symbols_[locate] = std::string(symbol);
}

std::string_view SymbolDirectory::symbol(std::uint16_t locate) const {
  if (locate >= symbols_.size()) {
    return {};
  }
  return symbols_[locate];
}

std::optional<std::uint16_t> SymbolDirectory::find(std::string_view symbol) const {
  if (symbol.empty()) {
    return std::nullopt;  // empty slots are "", so "" would match an unnamed locate
  }
  for (std::size_t locate = 1; locate < symbols_.size(); ++locate) {
    if (symbols_[locate] == symbol) {
      return static_cast<std::uint16_t>(locate);
    }
  }
  return std::nullopt;
}

}  // namespace itchbook
