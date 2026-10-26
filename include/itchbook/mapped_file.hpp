#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace itchbook {

// A whole file mapped read-only into memory. The constructor opens and maps
// it, and the destructor unmaps and closes it, so the bytes stay valid for as
// long as the MappedFile exists.
//
// Exactly one MappedFile owns a mapping. Copying is deleted, since two copies
// would unmap and close the same things twice. Moving hands the mapping over
// and leaves the moved-from object empty, like a default-constructed one.
class MappedFile {
 public:
  // Throws std::filesystem::filesystem_error if path cannot be opened, sized,
  // or mapped, or is not a regular file: a pipe such as /dev/stdin has no
  // size to map. An empty file is not mapped: data() is nullptr and size() is 0.
  explicit MappedFile(const std::filesystem::path& path);
  ~MappedFile();

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  // noexcept: moving only swaps a few numbers and cannot fail. Containers such
  // as std::vector only move elements when they grow if the move is noexcept.
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;

  // The file's bytes. Read-only: writing through them would crash.
  const std::uint8_t* data() const { return data_; }
  std::size_t size() const { return size_; }

  // The same bytes as one object: a pointer and a length that travel together.
  // Valid only while this MappedFile exists.
  std::span<const std::uint8_t> bytes() const { return {data_, size_}; }

 private:
  // Unmaps and closes whatever this object owns, then marks it empty.
  void release() noexcept;

  int fd_ = -1;                   // file descriptor from open
  std::uint8_t* data_ = nullptr;  // start of the mapping; munmap needs it non-const
  std::size_t size_ = 0;
};

}  // namespace itchbook
