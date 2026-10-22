#include "keel/btree.hpp"
#include "keel/buffer_pool.hpp"
#include "keel/disk_manager.hpp"
#include "keel/heap.hpp"
#include "keel/page.hpp"
#include "keel/store.hpp"
#include "keel/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <random>
#include <set>
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

struct Engine {
  explicit Engine(const std::string& path, std::size_t frames = 32)
      : disk(path, false), pool(disk, frames, keel::EvictPolicy::Clock, {}), store(pool, SealDirty) {}
  keel::DiskManager disk;
  keel::BufferPool pool;
  keel::Store store;
};

std::vector<std::string> ScanAll(keel::BTree& tree) {
  std::vector<std::string> keys;
  tree.Scan(nullptr, [&](const std::string& key, keel::RID) {
    keys.push_back(key);
    return true;
  });
  return keys;
}

}  // namespace

TEST(Values, IntegerAndStringOrder) {
  const std::string neg = keel::EncodeValues({keel::Value::Int(-2)});
  const std::string neg1 = keel::EncodeValues({keel::Value::Int(-1)});
  const std::string zero = keel::EncodeValues({keel::Value::Int(0)});
  const std::string pos = keel::EncodeValues({keel::Value::Int(5)});
  EXPECT_LT(keel::CompareBytes(neg, neg1), 0);
  EXPECT_LT(keel::CompareBytes(neg1, zero), 0);
  EXPECT_LT(keel::CompareBytes(zero, pos), 0);

  const std::string a = keel::EncodeValues({keel::Value::Varchar("a")});
  const std::string aa = keel::EncodeValues({keel::Value::Varchar("aa")});
  const std::string b = keel::EncodeValues({keel::Value::Varchar("b")});
  const std::string nul = keel::EncodeValues({keel::Value::Null(keel::Type::Varchar)});
  EXPECT_LT(keel::CompareBytes(nul, a), 0);
  EXPECT_LT(keel::CompareBytes(a, aa), 0);
  EXPECT_LT(keel::CompareBytes(aa, b), 0);

  keel::RID rid{9, 3};
  const std::string full = keel::WithRid(pos, rid);
  EXPECT_EQ(keel::UserKeyOf(full), pos);
  EXPECT_LT(keel::CompareBytes(pos, full), 0);
  EXPECT_LT(keel::CompareBytes(keel::WithRid(zero, rid), pos), 0);
}

TEST(Values, TupleRoundTrip) {
  keel::Schema schema = {
      {"id", keel::Type::Integer, 0, true, true, ""},
      {"name", keel::Type::Varchar, 0, false, false, ""},
      {"score", keel::Type::Double, 0, false, false, ""},
      {"ok", keel::Type::Boolean, 0, false, false, ""},
  };
  keel::Tuple row = {keel::Value::Int(7), keel::Value::Varchar("ada"), keel::Value::Int(3),
                     keel::Value::Bool(true)};
  auto bytes = keel::SerializeTuple(schema, row);
  auto back = keel::DeserializeTuple(schema, bytes.data(), bytes.size());
  ASSERT_EQ(back.size(), 4u);
  EXPECT_EQ(back[0].i, 7);
  EXPECT_EQ(back[1].s, "ada");
  EXPECT_DOUBLE_EQ(back[2].d, 3.0);
  EXPECT_TRUE(back[3].b);
}

TEST(HeapFile, InsertScanDeleteAcrossPages) {
  const std::string path = TempPath("heap");
  keel::PageId first = keel::kInvalidPage;
  keel::PageId last = keel::kInvalidPage;
  std::vector<keel::RID> rids;
  {
    Engine engine(path, 8);
    for (int i = 0; i < 500; ++i) {
      std::string payload = "row-" + std::to_string(i) + std::string(40, 'x');
      rids.push_back(keel::HeapInsert(engine.store, first, last,
                                      reinterpret_cast<const uint8_t*>(payload.data()),
                                      static_cast<uint16_t>(payload.size())));
    }
    EXPECT_NE(first, last);
    int seen = 0;
    keel::HeapForEach(engine.store, first, [&](keel::RID, std::string_view bytes) {
      ++seen;
      EXPECT_EQ(bytes.substr(0, 4), "row-");
      return true;
    });
    EXPECT_EQ(seen, 500);
    for (int i = 0; i < 500; i += 2) {
      EXPECT_TRUE(keel::HeapDelete(engine.store, rids[i]));
    }
    engine.pool.FlushAll();
  }
  Engine engine(path, 8);
    int seen = 0;
    keel::HeapForEach(engine.store, first, [&](keel::RID, std::string_view bytes) {
      ++seen;
      EXPECT_EQ(bytes.substr(0, 4), "row-");
      return true;
    });
  EXPECT_EQ(seen, 250);
  std::filesystem::remove(path);
}

TEST(BTree, RandomInsertRemoveAndReopen) {
  const std::string path = TempPath("btree");
  keel::PageId root = keel::kInvalidPage;
  std::mt19937 rng(42);
  std::set<int> live;
  {
    Engine engine(path, 8);
    root = keel::BTree::Create(engine.store);
    keel::BTree tree(engine.store, root);
    std::uniform_int_distribution<int> dist(0, 4000);
    for (int i = 0; i < 1500; ++i) {
      const int key = dist(rng);
      const std::string encoded = keel::WithRid(keel::EncodeValues({keel::Value::Int(key)}),
                                                keel::RID{1, static_cast<uint16_t>(i)});
      if (live.insert(key).second) {
        tree.Insert(encoded, keel::RID{1, static_cast<uint16_t>(i)});
      }
    }
    std::vector<std::string> present = ScanAll(tree);
    ASSERT_EQ(present.size(), live.size());
    for (std::size_t i = 0; i + 1 < present.size(); ++i) {
      EXPECT_LT(keel::CompareBytes(present[i], present[i + 1]), 0);
    }
    std::vector<std::string> doomed(present.begin(), present.begin() + static_cast<std::ptrdiff_t>(present.size() / 3));
    for (const auto& key : doomed) {
      EXPECT_TRUE(tree.Remove(key));
    }
    present = ScanAll(tree);
    EXPECT_EQ(present.size(), live.size() - doomed.size());
    const std::string start = present[present.size() / 2];
    std::vector<std::string> tail;
    tree.Scan(&start, [&](const std::string& key, keel::RID) {
      tail.push_back(key);
      return true;
    });
    ASSERT_FALSE(tail.empty());
    EXPECT_EQ(tail.front(), start);
    engine.pool.FlushAll();
  }
  Engine engine(path, 8);
  keel::BTree tree(engine.store, root);
  const auto present = ScanAll(tree);
  EXPECT_GT(present.size(), 100u);
  for (std::size_t i = 0; i + 1 < present.size(); ++i) {
    EXPECT_LT(keel::CompareBytes(present[i], present[i + 1]), 0);
  }
  std::filesystem::remove(path);
}

TEST(BTree, DescendingInsertsSplitTheRoot) {
  const std::string path = TempPath("desc");
  Engine engine(path, 16);
  keel::PageId root = keel::BTree::Create(engine.store);
  keel::BTree tree(engine.store, root);
  constexpr int n = 800;
  for (int i = n; i >= 1; --i) {
    const std::string key = keel::EncodeValues({keel::Value::Int(i)});
    tree.Insert(keel::WithRid(key, keel::RID{2, static_cast<uint16_t>(i)}), keel::RID{2, static_cast<uint16_t>(i)});
  }
  EXPECT_NE(root, keel::kInvalidPage);
  const auto keys = ScanAll(tree);
  ASSERT_EQ(keys.size(), static_cast<std::size_t>(n));
  EXPECT_EQ(keel::UserKeyOf(keys.front()), keel::EncodeValues({keel::Value::Int(1)}));
  EXPECT_EQ(keel::UserKeyOf(keys.back()), keel::EncodeValues({keel::Value::Int(n)}));
  std::filesystem::remove(path);
}
