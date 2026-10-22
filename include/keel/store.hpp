#pragma once

#include "keel/buffer_pool.hpp"
#include "keel/page.hpp"

#include <functional>

namespace keel {

// The only way heap pages and B+tree pages get mutated. The hook sees the
// pre-image and the already-modified page so it can assign an LSN, seal the
// checksum, and append a WAL record before the page is eligible for eviction.
class Store {
 public:
  using Hook = std::function<void(PageGuard& page, const char* before)>;

  Store(BufferPool& pool, Hook hook);

  PageGuard Fetch(PageId id);
  PageGuard NewPage(PageType type);
  void Update(PageGuard& page, const std::function<void(char*)>& mutate);

 private:
  BufferPool& pool_;
  Hook hook_;
};

}  // namespace keel
