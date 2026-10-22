#pragma once

#include "keel/config.hpp"

#include <string>

namespace keel {

// Owns the database file. Pages are addressed by id and transferred with
// pread/pwrite so a buffered iostream cannot write back a page we discarded.
class DiskManager {
 public:
  DiskManager(std::string path, bool sync);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  void Read(PageId id, char* out);
  // Recovery uses this when a checksum may be bad or the page may not exist yet.
  // Returns false if the page is absent or shorter than a page.
  bool TryRead(PageId id, char* out);
  void Write(PageId id, const char* in);
  PageId Allocate();
  uint32_t page_count() const { return pages_; }
  void Sync();
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int fd_{-1};
  uint32_t pages_{0};
  bool sync_{true};
};

}  // namespace keel
