#include "keel/database.hpp"
#include "keel/page.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <fcntl.h>
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
  std::filesystem::remove(std::string(path) + ".wal");
  return path.string();
}

keel::Options Fast() {
  keel::Options options;
  options.sync = false;
  options.buffer_pages = 32;
  return options;
}

void ExpectOk(const std::vector<keel::QueryResult>& results) {
  for (const auto& result : results) {
    ASSERT_TRUE(result.ok) << result.error;
  }
}

}  // namespace

TEST(Recovery, CommittedInsertSurvivesCrashBeforeDataFlush) {
  const std::string path = TempPath("redo_sql");
  {
    keel::Database db(path, Fast());
    ExpectOk(db.Execute("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16));"));
    ExpectOk(db.Execute("INSERT INTO t VALUES (1, 'ada'), (2, 'bea');"));
    db.Abandon();
  }
  keel::Database db(path, Fast());
  auto rows = db.Execute("SELECT name FROM t ORDER BY id;");
  ExpectOk(rows);
  ASSERT_EQ(rows[0].rows.size(), 2u);
  EXPECT_EQ(keel::FormatValue(rows[0].rows[0][0]), "ada");
  EXPECT_EQ(keel::FormatValue(rows[0].rows[1][0]), "bea");
}

TEST(Recovery, UncommittedInsertIsUndoneAfterStolenPage) {
  const std::string path = TempPath("undo_sql");
  {
    keel::Database db(path, Fast());
    ExpectOk(db.Execute("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16));"));
    ExpectOk(db.Execute("INSERT INTO t VALUES (1, 'ada');"));
    ExpectOk(db.Execute("BEGIN; INSERT INTO t VALUES (2, 'ghost');"));
    db.FlushDirty();
    db.Abandon();
  }
  keel::Database db(path, Fast());
  auto rows = db.Execute("SELECT id FROM t ORDER BY id;");
  ExpectOk(rows);
  ASSERT_EQ(rows[0].rows.size(), 1u);
  EXPECT_EQ(keel::FormatValue(rows[0].rows[0][0]), "1");
}

TEST(Recovery, TornHeapPageIsRepairedFromTheLog) {
  const std::string path = TempPath("torn_sql");
  keel::PageId heap = keel::kInvalidPage;
  {
    keel::Database db(path, Fast());
    ExpectOk(db.Execute("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16));"));
    ExpectOk(db.Execute("INSERT INTO t VALUES (7, 'kept');"));
    db.FlushDirty();
    const int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    const off_t end = ::lseek(fd, 0, SEEK_END);
    ASSERT_GT(end, 0);
    const uint32_t pages = static_cast<uint32_t>(end / static_cast<off_t>(keel::kPageSize));
    std::vector<char> page(keel::kPageSize);
    for (uint32_t id = 0; id < pages; ++id) {
      ASSERT_EQ(::pread(fd, page.data(), keel::kPageSize, static_cast<off_t>(id) * keel::kPageSize),
                static_cast<ssize_t>(keel::kPageSize));
      if (keel::PageGetType(page.data()) == keel::PageType::Heap) {
        heap = id;
        page[keel::kPageHeaderSize + 4] ^= 0x5A;
        ASSERT_EQ(::pwrite(fd, page.data(), keel::kPageSize, static_cast<off_t>(id) * keel::kPageSize),
                  static_cast<ssize_t>(keel::kPageSize));
        break;
      }
    }
    ::close(fd);
    ASSERT_NE(heap, keel::kInvalidPage);
    db.Abandon();
  }
  keel::Database db(path, Fast());
  auto rows = db.Execute("SELECT name FROM t WHERE id = 7;");
  ExpectOk(rows);
  ASSERT_EQ(rows[0].rows.size(), 1u);
  EXPECT_EQ(keel::FormatValue(rows[0].rows[0][0]), "kept");
  db.FlushDirty();
  const int fd = ::open(path.c_str(), O_RDONLY);
  ASSERT_GE(fd, 0);
  std::vector<char> page(keel::kPageSize);
  ASSERT_EQ(::pread(fd, page.data(), keel::kPageSize, static_cast<off_t>(heap) * keel::kPageSize),
            static_cast<ssize_t>(keel::kPageSize));
  ::close(fd);
  EXPECT_TRUE(keel::PageChecksumOk(page.data()));
}

TEST(Recovery, CleanShutdownReopens) {
  const std::string path = TempPath("clean_sql");
  {
    keel::Database db(path, Fast());
    ExpectOk(db.Execute("CREATE TABLE t (id INT PRIMARY KEY); INSERT INTO t VALUES (1), (2), (3);"));
  }
  keel::Database db(path, Fast());
  auto rows = db.Execute("SELECT COUNT(*) FROM t;");
  ExpectOk(rows);
  EXPECT_EQ(keel::FormatValue(rows[0].rows[0][0]), "3");
}
