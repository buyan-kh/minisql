#pragma once

#include "keel/config.hpp"
#include "keel/crc32.hpp"
#include "keel/error.hpp"

#include <cstring>
#include <string>

namespace keel {

enum class PageType : uint8_t {
  Free = 0,
  Meta = 1,
  Heap = 2,
  Leaf = 3,
  Internal = 4,
  Catalog = 5,
};

inline uint8_t ReadU8(const char* p) { return static_cast<uint8_t>(p[0]); }
inline uint16_t ReadU16(const char* p) {
  uint16_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint32_t ReadU32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint64_t ReadU64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline void WriteU8(char* p, uint8_t v) { p[0] = static_cast<char>(v); }
inline void WriteU16(char* p, uint16_t v) { std::memcpy(p, &v, sizeof(v)); }
inline void WriteU32(char* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
inline void WriteU64(char* p, uint64_t v) { std::memcpy(p, &v, sizeof(v)); }

// Header layout (32 bytes), little-endian:
//  0 u32 page id
//  4 u8  type
//  5 u8  flags
//  6 u16 slot count
//  8 u16 free-space start (grows up)
// 10 u16 slot-directory start (grows down)
// 12 u32 next page (heap chain or leaf sibling)
// 16 u32 extra (internal-node rightmost child)
// 20 u64 log sequence number
// 28 u32 checksum over the page with this field zeroed
inline PageId PageGetId(const char* p) { return ReadU32(p + 0); }
inline PageType PageGetType(const char* p) { return static_cast<PageType>(ReadU8(p + 4)); }
inline uint16_t PageGetSlots(const char* p) { return ReadU16(p + 6); }
inline uint16_t PageGetFreeBegin(const char* p) { return ReadU16(p + 8); }
inline uint16_t PageGetFreeEnd(const char* p) { return ReadU16(p + 10); }
inline PageId PageGetNext(const char* p) { return ReadU32(p + 12); }
inline PageId PageGetExtra(const char* p) { return ReadU32(p + 16); }
inline LSN PageGetLsn(const char* p) { return ReadU64(p + 20); }

inline void PageSetId(char* p, PageId id) { WriteU32(p + 0, id); }
inline void PageSetType(char* p, PageType t) { WriteU8(p + 4, static_cast<uint8_t>(t)); }
inline void PageSetSlots(char* p, uint16_t n) { WriteU16(p + 6, n); }
inline void PageSetFreeBegin(char* p, uint16_t v) { WriteU16(p + 8, v); }
inline void PageSetFreeEnd(char* p, uint16_t v) { WriteU16(p + 10, v); }
inline void PageSetNext(char* p, PageId id) { WriteU32(p + 12, id); }
inline void PageSetExtra(char* p, PageId id) { WriteU32(p + 16, id); }
inline void PageSetLsn(char* p, LSN lsn) { WriteU64(p + 20, lsn); }

inline uint32_t PageComputeChecksum(const char* page) {
  char tmp[kPageSize];
  std::memcpy(tmp, page, kPageSize);
  WriteU32(tmp + 28, 0);
  return Crc32(tmp, kPageSize);
}

inline void SealPage(char* page) {
  WriteU32(page + 28, 0);
  WriteU32(page + 28, Crc32(page, kPageSize));
}

inline bool PageChecksumOk(const char* page) {
  const uint32_t stored = ReadU32(page + 28);
  return stored == PageComputeChecksum(page) && stored != 0;
}

inline void InitPage(char* page, PageId id, PageType type) {
  std::memset(page, 0, kPageSize);
  PageSetId(page, id);
  PageSetType(page, type);
  PageSetSlots(page, 0);
  PageSetFreeBegin(page, static_cast<uint16_t>(kPageHeaderSize));
  PageSetFreeEnd(page, static_cast<uint16_t>(kPageSize));
  PageSetNext(page, kInvalidPage);
  PageSetExtra(page, kInvalidPage);
  PageSetLsn(page, 0);
  SealPage(page);
}

inline uint16_t PageFreeSpace(const char* page) {
  return static_cast<uint16_t>(PageGetFreeEnd(page) - PageGetFreeBegin(page));
}

// Slot i lives at the end of the page. offset==0 && length==0 means a dead slot.
// A live record may also start at 0 only if we ever placed one there; we never do
// because the header occupies the first 32 bytes, so offset 0 unambiguously means free.
inline char* SlotAddr(char* page, uint16_t slot) {
  return page + kPageSize - (static_cast<std::size_t>(slot) + 1) * 4;
}
inline const char* SlotAddr(const char* page, uint16_t slot) {
  return page + kPageSize - (static_cast<std::size_t>(slot) + 1) * 4;
}

inline uint16_t SlotOffset(const char* page, uint16_t slot) { return ReadU16(SlotAddr(page, slot)); }
inline uint16_t SlotLength(const char* page, uint16_t slot) { return ReadU16(SlotAddr(page, slot) + 2); }

inline void SlotSet(char* page, uint16_t slot, uint16_t offset, uint16_t length) {
  char* a = SlotAddr(page, slot);
  WriteU16(a, offset);
  WriteU16(a + 2, length);
}

inline const char* kMagic = "KEELDB01";

inline bool MetaMagicOk(const char* page) {
  return PageGetType(page) == PageType::Meta &&
         std::memcmp(page + kPageHeaderSize, kMagic, 8) == 0;
}

}  // namespace keel
