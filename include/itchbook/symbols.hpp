#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace itchbook {

// Maps stock locate codes to symbols, filled from R messages. Locates start
// at 1, stay fixed for the day, and have no gaps, so the locate is used
// directly as the vector index. Slot 0 is never used.
class SymbolDirectory {
 public:
  // Stores a copy of symbol. The copy matters: symbol usually points into the
  // message buffer, which is overwritten by the next message.
  void add(std::uint16_t locate, std::string_view symbol);

  // Returns "" for a locate that no R message has named.
  std::string_view symbol(std::uint16_t locate) const;

  // The locate named symbol, or nullopt. Scans every slot, so it is for
  // occasional lookups by name, not for the per-message path.
  std::optional<std::uint16_t> find(std::string_view symbol) const;

  // How many locates have a symbol.
  std::size_t size() const { return count_; }

 private:
  std::vector<std::string> symbols_;
  std::size_t count_ = 0;
};

}  // namespace itchbook
