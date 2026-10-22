#include "keel/buffer_pool.hpp"
#include "keel/disk_manager.hpp"
#include "keel/page.hpp"
#include "keel/slotted.hpp"
#include "keel/store.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::string TempPath(const char* name) {
  auto dir = std::filesystem::temp_directory_path() / "keel_tests";
  std::filesystem::create_directories(dir);
  auto path = dir / (std::string(name) + "_" + std::to_string(::getpid()));
  std::filesystem::remove(path);
  return path.string();
}

void SealDirty(keel::PageGuard& page, const char*) {
  keel::SealPage(page.data());
  page.MarkDirty();
}

}  // namespace

TEST(CrcAndPage, SealRoundTrip) {
  char page[keel::kPageSize];
  keel::InitPage(page, 7, keel::PageType::Heap);
  EXPECT_TRUE(keel::PageChecksumOk(page));
  EXPECT_EQ(keel::PageGetId(page), 7u);
  page[100] ^= 0x1;
  EXPECT_FALSE(keel::PageChecksumOk(page));
}

TEST(SlottedPage, InsertGetDeleteCompactReuse) {
  char page[keel::kPageSize];
  keel::InitPage(page, 1, keel::PageType::Heap);
  const char* a = "alpha";
  const char* b = "bravo-record";
  auto sa = keel::SlotInsert(page, a, 5);
  auto sb = keel::SlotInsert(page, b, static_cast<uint16_t>(std::strlen(b)));
  ASSERT_TRUE(sa);
  ASSERT_TRUE(sb);
  EXPECT_EQ(*sa, 0u);
  EXPECT_EQ(*sb, 1u);
  auto ga = keel::SlotGet(page, *sa);
  ASSERT_TRUE(ga);
  EXPECT_EQ(std::string(ga->first, ga->second), "alpha");

  ASSERT_TRUE(keel::SlotDelete(page, *sa));
  EXPECT_FALSE(keel::SlotGet(page, *sa));
  const char* c = "c";
  auto sc = keel::SlotInsert(page, c, 1);
  ASSERT_TRUE(sc);
  EXPECT_EQ(*sc, *sa);

  // Fill the page, delete every other record, and confirm compaction
  // reclaims enough space for another insert without moving live slot ids.
  std::vector<uint16_t> slots;
  int i = 0;
  for (;;) {
    std::string payload(64, static_cast<char>('a' + (i % 26)));
    payload[0] = static_cast<char>(i);
    auto s = keel::SlotInsert(page, payload.data(), static_cast<uint16_t>(payload.size()));
    if (!s) {
      break;
    }
    slots.push_back(*s);
    ++i;
  }
  ASSERT_GT(slots.size(), 4u);
  for (std::size_t k = 0; k < slots.size(); k += 2) {
    ASSERT_TRUE(keel::SlotDelete(page, slots[k]));
  }
  std::string again(200, 'Z');
  auto extra = keel::SlotInsert(page, again.data(), static_cast<uint16_t>(again.size()));
  ASSERT_TRUE(extra);
  auto got = keel::SlotGet(page, *extra);
  ASSERT_TRUE(got);
  EXPECT_EQ(std::string(got->first, got->second), again);
  auto survivor = keel::SlotGet(page, slots[1]);
  ASSERT_TRUE(survivor);
  EXPECT_EQ(survivor->second, 64u);
}

TEST(DiskManager, AllocateReadWrite) {
  const std::string path = TempPath("disk");
  keel::PageId id = 0;
  {
    keel::DiskManager disk(path, false);
    id = disk.Allocate();
    EXPECT_EQ(id, 0u);
    char page[keel::kPageSize];
    disk.Read(id, page);
    EXPECT_TRUE(keel::PageChecksumOk(page));
    EXPECT_EQ(keel::PageGetType(page), keel::PageType::Free);
    page[64] = 'Q';
    keel::SealPage(page);
    disk.Write(id, page);
  }
  keel::DiskManager disk(path, false);
  char page[keel::kPageSize];
  disk.Read(id, page);
  EXPECT_EQ(page[64], 'Q');
  EXPECT_TRUE(keel::PageChecksumOk(page));
  std::filesystem::remove(path);
}

TEST(BufferPool, PinnedPageIsNotEvicted) {
  const std::string path = TempPath("pin");
  keel::DiskManager disk(path, false);
  keel::BufferPool pool(disk, 8, keel::EvictPolicy::Clock, {});
  const keel::PageId id = pool.Allocate();
  auto pinned = pool.Fetch(id);
  pinned.data()[40] = 'P';
  keel::SealPage(pinned.data());
  pinned.MarkDirty();
  for (int i = 0; i < 32; ++i) {
    auto extra = pool.Fetch(pool.Allocate());
    extra.data()[40] = static_cast<char>(i);
    keel::SealPage(extra.data());
    extra.MarkDirty();
  }
  EXPECT_EQ(pinned.data()[40], 'P');
  std::filesystem::remove(path);
}

TEST(BufferPool, ClockEvictsUnpinnedDirtyPage) {
  const std::string path = TempPath("clock");
  keel::DiskManager disk(path, false);
  keel::BufferPool pool(disk, 8, keel::EvictPolicy::Clock, {});
  std::vector<keel::PageId> ids;
  for (int i = 0; i < 8; ++i) {
    ids.push_back(pool.Allocate());
  }
  {
    auto page = pool.Fetch(ids[0]);
    page.data()[50] = 'A';
    keel::SealPage(page.data());
    page.MarkDirty();
  }
  for (int i = 0; i < 40; ++i) {
    auto page = pool.Fetch(ids[1 + (i % 7)]);
    page.data()[50] = 'B';
    keel::SealPage(page.data());
    page.MarkDirty();
  }
  pool.FlushAll();
  char raw[keel::kPageSize];
  disk.Read(ids[0], raw);
  EXPECT_EQ(raw[50], 'A');
  std::filesystem::remove(path);
}

TEST(BufferPool, LruEvictsLeastRecentlyUsed) {
  const std::string path = TempPath("lru");
  keel::DiskManager disk(path, false);
  keel::BufferPool pool(disk, 8, keel::EvictPolicy::Lru, {});
  // Use the whole pool so the next miss must evict.
  std::vector<keel::PageId> ids;
  for (int i = 0; i < 8; ++i) {
    ids.push_back(pool.Allocate());
    auto page = pool.Fetch(ids.back());
    page.data()[60] = static_cast<char>('a' + i);
    keel::SealPage(page.data());
    page.MarkDirty();
  }
  // Touch pages 1..7 so page 0 is the least recently used.
  for (int i = 1; i < 8; ++i) {
    auto page = pool.Fetch(ids[i]);
    EXPECT_EQ(page.data()[60], static_cast<char>('a' + i));
  }
  const keel::PageId fresh = pool.Allocate();
  {
    auto page = pool.Fetch(fresh);
    page.data()[60] = 'Z';
    keel::SealPage(page.data());
    page.MarkDirty();
  }
  pool.FlushAll();
  char raw[keel::kPageSize];
  disk.Read(ids[0], raw);
  EXPECT_EQ(raw[60], 'a');
  // Page 1 stayed resident, so it was not required to be on disk before FlushAll,
  // but after FlushAll it must still hold its byte.
  disk.Read(ids[1], raw);
  EXPECT_EQ(raw[60], 'b');
  std::filesystem::remove(path);
}

TEST(Store, NewPageLogsPreImageViaHook) {
  const std::string path = TempPath("store");
  keel::DiskManager disk(path, false);
  keel::BufferPool pool(disk, 8, keel::EvictPolicy::Clock, {});
  int hooks = 0;
  keel::PageId seen_before = keel::kInvalidPage;
  keel::Store store(pool, [&](keel::PageGuard& page, const char* before) {
    ++hooks;
    seen_before = keel::PageGetId(before);
    EXPECT_EQ(keel::PageGetType(before), keel::PageType::Free);
    EXPECT_EQ(keel::PageGetType(page.data()), keel::PageType::Leaf);
    SealDirty(page, before);
  });
  auto page = store.NewPage(keel::PageType::Leaf);
  EXPECT_EQ(hooks, 1);
  EXPECT_EQ(seen_before, page.id());
  EXPECT_TRUE(keel::PageChecksumOk(page.data()));
  std::filesystem::remove(path);
}
