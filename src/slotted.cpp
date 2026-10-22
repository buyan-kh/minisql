#include "keel/slotted.hpp"

#include "keel/error.hpp"

#include <cstring>
#include <vector>

namespace keel {
namespace {

uint16_t NeedBytes(uint16_t len, bool new_slot) {
  return static_cast<uint16_t>(len + (new_slot ? 4 : 0));
}

}  // namespace

void SlotCompact(char* page) {
  const uint16_t n = PageGetSlots(page);
  struct Live {
    uint16_t slot;
    std::vector<char> bytes;
  };
  std::vector<Live> live;
  live.reserve(n);
  for (uint16_t i = 0; i < n; ++i) {
    const uint16_t len = SlotLength(page, i);
    const uint16_t off = SlotOffset(page, i);
    if (len == 0) {
      continue;
    }
    Live rec;
    rec.slot = i;
    rec.bytes.assign(page + off, page + off + len);
    live.push_back(std::move(rec));
  }
  std::memset(page + kPageHeaderSize, 0, kPageSize - kPageHeaderSize);
  uint16_t dest = static_cast<uint16_t>(kPageHeaderSize);
  for (uint16_t i = 0; i < n; ++i) {
    SlotSet(page, i, 0, 0);
  }
  for (const auto& rec : live) {
    std::memcpy(page + dest, rec.bytes.data(), rec.bytes.size());
    SlotSet(page, rec.slot, dest, static_cast<uint16_t>(rec.bytes.size()));
    dest = static_cast<uint16_t>(dest + rec.bytes.size());
  }
  PageSetFreeBegin(page, dest);
  PageSetFreeEnd(page, static_cast<uint16_t>(kPageSize - static_cast<std::size_t>(n) * 4));
}

std::optional<uint16_t> SlotInsert(char* page, const void* data, uint16_t len) {
  if (len == 0) {
    throw Error("refusing to store a zero-length record");
  }
  if (static_cast<std::size_t>(len) + 4 > kPageSize - kPageHeaderSize) {
    return std::nullopt;
  }
  const uint16_t n = PageGetSlots(page);
  int free_slot = -1;
  for (uint16_t i = 0; i < n; ++i) {
    if (SlotLength(page, i) == 0) {
      free_slot = static_cast<int>(i);
      break;
    }
  }
  const bool new_slot = free_slot < 0;
  if (PageFreeSpace(page) < NeedBytes(len, new_slot)) {
    SlotCompact(page);
    if (PageFreeSpace(page) < NeedBytes(len, new_slot)) {
      return std::nullopt;
    }
  }
  const uint16_t off = PageGetFreeBegin(page);
  std::memcpy(page + off, data, len);
  PageSetFreeBegin(page, static_cast<uint16_t>(off + len));
  if (new_slot) {
    SlotSet(page, n, off, len);
    PageSetSlots(page, static_cast<uint16_t>(n + 1));
    PageSetFreeEnd(page, static_cast<uint16_t>(PageGetFreeEnd(page) - 4));
    return n;
  }
  SlotSet(page, static_cast<uint16_t>(free_slot), off, len);
  return static_cast<uint16_t>(free_slot);
}

std::optional<std::pair<const char*, uint16_t>> SlotGet(const char* page, uint16_t slot) {
  if (slot >= PageGetSlots(page)) {
    return std::nullopt;
  }
  const uint16_t len = SlotLength(page, slot);
  if (len == 0) {
    return std::nullopt;
  }
  return std::make_pair(page + SlotOffset(page, slot), len);
}

bool SlotDelete(char* page, uint16_t slot) {
  if (slot >= PageGetSlots(page) || SlotLength(page, slot) == 0) {
    return false;
  }
  SlotSet(page, slot, 0, 0);
  return true;
}

}  // namespace keel
