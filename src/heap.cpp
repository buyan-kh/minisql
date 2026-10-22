#include "keel/heap.hpp"

#include "keel/error.hpp"
#include "keel/slotted.hpp"

namespace keel {
namespace {

std::optional<uint16_t> InsertInto(Store& store, PageGuard& page, const uint8_t* data, uint16_t len) {
  if (PageGetType(page.data()) != PageType::Heap) {
    throw Error("heap operation on a non-heap page");
  }
  std::optional<uint16_t> slot;
  store.Update(page, [&](char* raw) { slot = SlotInsert(raw, data, len); });
  return slot;
}

}  // namespace

RID HeapInsert(Store& store, PageId& first, PageId& last, const uint8_t* data, uint16_t len) {
  if (static_cast<std::size_t>(len) + 4 > kPageSize - kPageHeaderSize) {
    throw Error("tuple does not fit in a page");
  }
  if (first == kInvalidPage) {
    PageGuard page = store.NewPage(PageType::Heap);
    auto slot = InsertInto(store, page, data, len);
    if (!slot) {
      throw Error("tuple does not fit in an empty heap page");
    }
    first = last = page.id();
    return RID{page.id(), *slot};
  }
  {
    PageGuard page = store.Fetch(last);
    if (auto slot = InsertInto(store, page, data, len)) {
      return RID{last, *slot};
    }
  }
  PageGuard created = store.NewPage(PageType::Heap);
  const PageId created_id = created.id();
  {
    PageGuard tail = store.Fetch(last);
    store.Update(tail, [&](char* raw) { PageSetNext(raw, created_id); });
  }
  auto slot = InsertInto(store, created, data, len);
  if (!slot) {
    throw Error("tuple does not fit in an empty heap page");
  }
  last = created_id;
  return RID{created_id, *slot};
}

std::optional<std::vector<uint8_t>> HeapRead(Store& store, RID rid) {
  PageGuard page = store.Fetch(rid.page);
  if (PageGetType(page.data()) != PageType::Heap) {
    throw Error("heap read on a non-heap page");
  }
  auto rec = SlotGet(page.data(), rid.slot);
  if (!rec) {
    return std::nullopt;
  }
  return std::vector<uint8_t>(rec->first, rec->first + rec->second);
}

bool HeapDelete(Store& store, RID rid) {
  PageGuard page = store.Fetch(rid.page);
  if (PageGetType(page.data()) != PageType::Heap) {
    throw Error("heap delete on a non-heap page");
  }
  bool ok = false;
  store.Update(page, [&](char* raw) { ok = SlotDelete(raw, rid.slot); });
  return ok;
}

void HeapForEach(Store& store, PageId first,
                 const std::function<bool(RID rid, std::string_view bytes)>& visit) {
  PageId cur = first;
  std::size_t guard = 0;
  while (cur != kInvalidPage) {
    if (++guard > 10000000) {
      throw Error("heap page chain is cyclic or unbounded");
    }
    PageGuard page = store.Fetch(cur);
    const uint16_t n = PageGetSlots(page.data());
    const PageId next = PageGetNext(page.data());
    for (uint16_t slot = 0; slot < n; ++slot) {
      auto rec = SlotGet(page.data(), slot);
      if (!rec) {
        continue;
      }
      if (!visit(RID{cur, slot}, std::string_view(rec->first, rec->second))) {
        return;
      }
    }
    cur = next;
  }
}

}  // namespace keel
