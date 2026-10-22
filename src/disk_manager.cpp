#include "keel/disk_manager.hpp"

#include "keel/error.hpp"
#include "keel/page.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <mutex>
#include <vector>

namespace keel {
namespace {

std::mutex& DiskMu() {
  static std::mutex mu;
  return mu;
}

}  // namespace

DiskManager::DiskManager(std::string path, bool sync) : path_(std::move(path)), sync_(sync) {
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) {
    throw Error("failed to open database file " + path_);
  }
  const off_t end = ::lseek(fd_, 0, SEEK_END);
  if (end < 0) {
    throw Error("failed to stat database file " + path_);
  }
  if (end % static_cast<off_t>(kPageSize) != 0) {
    throw Error("database file size is not a multiple of the page size");
  }
  pages_ = static_cast<uint32_t>(end / static_cast<off_t>(kPageSize));
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void DiskManager::Read(PageId id, char* out) {
  if (!TryRead(id, out)) {
    throw Error("short read on page " + std::to_string(id));
  }
}

bool DiskManager::TryRead(PageId id, char* out) {
  std::lock_guard lock(DiskMu());
  if (id >= pages_) {
    return false;
  }
  const off_t off = static_cast<off_t>(id) * static_cast<off_t>(kPageSize);
  std::size_t got = 0;
  while (got < kPageSize) {
    const ssize_t n = ::pread(fd_, out + got, kPageSize - got, off + static_cast<off_t>(got));
    if (n < 0) {
      throw Error("read failed on page " + std::to_string(id));
    }
    if (n == 0) {
      return false;
    }
    got += static_cast<std::size_t>(n);
  }
  return true;
}

void DiskManager::Write(PageId id, const char* in) {
  std::lock_guard lock(DiskMu());
  const off_t off = static_cast<off_t>(id) * static_cast<off_t>(kPageSize);
  std::size_t put = 0;
  while (put < kPageSize) {
    const ssize_t n = ::pwrite(fd_, in + put, kPageSize - put, off + static_cast<off_t>(put));
    if (n < 0) {
      throw Error("write failed on page " + std::to_string(id));
    }
    put += static_cast<std::size_t>(n);
  }
  if (id >= pages_) {
    pages_ = id + 1;
  }
}

PageId DiskManager::Allocate() {
  std::lock_guard lock(DiskMu());
  const PageId id = pages_;
  std::vector<char> page(kPageSize);
  InitPage(page.data(), id, PageType::Free);
  const off_t off = static_cast<off_t>(id) * static_cast<off_t>(kPageSize);
  std::size_t put = 0;
  while (put < kPageSize) {
    const ssize_t n = ::pwrite(fd_, page.data() + put, kPageSize - put, off + static_cast<off_t>(put));
    if (n < 0) {
      throw Error("failed to extend database file");
    }
    put += static_cast<std::size_t>(n);
  }
  ++pages_;
  return id;
}

void DiskManager::Sync() {
  if (!sync_) {
    return;
  }
  std::lock_guard lock(DiskMu());
  if (::fsync(fd_) != 0) {
    throw Error("fsync failed on database file");
  }
}

}  // namespace keel
