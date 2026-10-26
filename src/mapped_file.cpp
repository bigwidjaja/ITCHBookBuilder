#include "itchbook/mapped_file.hpp"

#include <cerrno>
#include <system_error>
#include <utility>

#include <fcntl.h>     // open
#include <sys/mman.h>  // mmap, munmap, madvise
#include <sys/stat.h>  // fstat
#include <unistd.h>    // close

namespace itchbook {

namespace {

// The OS calls report failure through errno instead of throwing. err is
// passed in because close() can overwrite errno before we get here.
[[noreturn]] void fail(const char* what, const std::filesystem::path& path, int err) {
  throw std::filesystem::filesystem_error(what, path, std::error_code(err, std::generic_category()));
}

}  // namespace

// :: picks the OS functions in the global namespace. If the constructor
// throws, the destructor never runs, so each failure after open closes fd_
// itself before throwing.
MappedFile::MappedFile(const std::filesystem::path& path) {
  fd_ = ::open(path.c_str(), O_RDONLY);
  if (fd_ == -1) {
    fail("failed to open file", path, errno);
  }

  struct stat st;
  if (::fstat(fd_, &st) == -1) {
    const int err = errno;
    ::close(fd_);
    fail("failed to stat file", path, err);
  }
  // A pipe or terminal reports size 0, so mapping it would silently give no
  // bytes. Refuse it instead.
  if (!S_ISREG(st.st_mode)) {
    ::close(fd_);
    fail("not a regular file (pipes cannot be mapped; decompress to a file first)", path,
         EINVAL);
  }
  size_ = static_cast<std::size_t>(st.st_size);
  if (size_ == 0) {
    return;  // mmap rejects a length of 0
  }

  // nullptr lets the OS pick the address. PROT_READ makes the pages
  // read-only, MAP_PRIVATE keeps the mapping ours, and 0 starts at byte 0.
  void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
  if (p == MAP_FAILED) {
    const int err = errno;
    ::close(fd_);
    fail("failed to map file", path, err);
  }
  data_ = static_cast<std::uint8_t*>(p);

  // A hint that we read front to back, so the OS reads ahead. Only a hint,
  // so a failure here is ignored.
  ::madvise(p, size_, MADV_SEQUENTIAL);
}

MappedFile::~MappedFile() {
  release();
}

// std::exchange(x, v) sets x to v and returns x's old value: take the other
// object's resources and leave it empty in one step, so its destructor does
// nothing.
MappedFile::MappedFile(MappedFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  // `a = std::move(a)` must not release a's mapping and then take it back.
  if (this != &other) {
    release();  // this object's old mapping, if any, is released first
    fd_ = std::exchange(other.fd_, -1);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

void MappedFile::release() noexcept {
  if (data_ != nullptr) {
    ::munmap(data_, size_);
  }
  if (fd_ != -1) {
    ::close(fd_);
  }
  fd_ = -1;
  data_ = nullptr;
  size_ = 0;
}

}  // namespace itchbook
