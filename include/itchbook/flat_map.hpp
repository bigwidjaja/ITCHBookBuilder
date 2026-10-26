#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace itchbook {

// A hash map stored in one flat array, for integer keys.
//
// std::unordered_map allocates every entry as its own node and reaches it
// through a bucket pointer, so a lookup usually costs two cache misses. Here
// every entry sits in the array itself:
//
// - Open addressing with linear probing: a key goes in its home slot, or the
//   next free slot after it. A lookup walks forward from the home slot until
//   it finds the key or an empty slot.
// - Capacity is a power of two, so "slot % capacity" is "slot & mask".
// - The home slot comes from a multiplicative hash: multiply by 2^64 / phi
//   (0x9E3779B97F4A7C15) and keep the high bits. Multiplying mixes every bit
//   of the key into the high bits, so sequential keys like order numbers
//   spread out instead of filling one run.
// - The table grows (doubles and reinserts) before it is more than half full.
//   Above that, runs of full slots get long and lookups slow down.
// - Erase uses backward-shift deletion instead of tombstones. After a slot is
//   emptied, later entries in the same run slide back into the hole when that
//   keeps them reachable, so the table looks as if the key was never inserted.
//   The book erases almost every order it inserts (140M inserts, at most 1.9M
//   live), and tombstones would pile up and lengthen every lookup until a
//   rebuild. Backward shift never needs one.
//
// find() returns a pointer into the array. Like an iterator into a vector, it
// is invalidated by any insert (which may grow the table) or erase (which may
// move entries).
template <typename K, typename V>
class FlatMap {
 public:
  FlatMap() { rehash(min_capacity); }

  std::size_t size() const { return size_; }
  std::size_t capacity() const { return slots_.size(); }
  bool empty() const { return size_ == 0; }

  // Makes room for n entries without growing again.
  void reserve(std::size_t n) {
    const std::size_t needed = std::bit_ceil(n * 2);  // at most half full
    if (needed > capacity()) {
      rehash(needed);
    }
  }

  // The value for key, or nullptr.
  V* find(K key) {
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (!s.full) {
        return nullptr;
      }
      if (s.key == key) {
        return &s.value;
      }
    }
  }

  const V* find(K key) const { return const_cast<FlatMap*>(this)->find(key); }

  // Adds key with value. Returns false, and changes nothing, if key is
  // already there (like std::unordered_map::emplace).
  bool insert(K key, const V& value) {
    if ((size_ + 1) * 2 > capacity()) {
      rehash(capacity() * 2);
    }
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (!s.full) {
        s = Slot{key, value, true};
        ++size_;
        return true;
      }
      if (s.key == key) {
        return false;
      }
    }
  }

  // Removes key. Returns false if it was not there.
  bool erase(K key) {
    std::size_t hole = home(key);
    for (;; hole = (hole + 1) & mask_) {
      if (!slots_[hole].full) {
        return false;
      }
      if (slots_[hole].key == key) {
        break;
      }
    }
    // Backward shift: look at each entry after the hole until an empty slot.
    // An entry may move into the hole only if the hole is not before its home
    // slot, or a lookup starting at its home would never reach it. Measured
    // along the probe direction: move it if its distance from home is at
    // least its distance from the hole.
    for (std::size_t j = (hole + 1) & mask_; slots_[j].full; j = (j + 1) & mask_) {
      const std::size_t from_home = (j - home(slots_[j].key)) & mask_;
      const std::size_t from_hole = (j - hole) & mask_;
      if (from_home >= from_hole) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].full = false;
    --size_;
    return true;
  }

 private:
  struct Slot {
    K key{};
    V value{};
    bool full = false;
  };

  static constexpr std::size_t min_capacity = 16;

  std::size_t home(K key) const {
    return static_cast<std::size_t>((static_cast<std::uint64_t>(key) * 0x9E3779B97F4A7C15ull) >>
                                    shift_);
  }

  // Moves every entry into a new array of `capacity` slots.
  void rehash(std::size_t capacity) {
    std::vector<Slot> old = std::exchange(slots_, std::vector<Slot>(capacity));
    mask_ = capacity - 1;
    shift_ = 64 - std::countr_zero(capacity);  // keep log2(capacity) high bits
    size_ = 0;
    for (const Slot& s : old) {
      if (s.full) {
        insert(s.key, s.value);
      }
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  int shift_ = 64;
  std::size_t size_ = 0;
};

}  // namespace itchbook
