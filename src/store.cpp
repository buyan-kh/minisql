#include "keel/store.hpp"

#include <cstring>

namespace keel {

Store::Store(BufferPool& pool, Hook hook) : pool_(pool), hook_(std::move(hook)) {}

PageGuard Store::Fetch(PageId id) { return pool_.Fetch(id); }

PageGuard Store::NewPage(PageType type) {
  const PageId id = pool_.Allocate();
  PageGuard guard = pool_.Fetch(id);
  char before[kPageSize];
  std::memcpy(before, guard.data(), kPageSize);
  InitPage(guard.data(), id, type);
  hook_(guard, before);
  return guard;
}

void Store::Update(PageGuard& page, const std::function<void(char*)>& mutate) {
  char before[kPageSize];
  std::memcpy(before, page.data(), kPageSize);
  mutate(page.data());
  if (std::memcmp(before, page.data(), kPageSize) == 0) {
    return;
  }
  hook_(page, before);
}

}  // namespace keel
