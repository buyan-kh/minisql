#include "keel/disk_manager.hpp"
#include "keel/page.hpp"
#include "keel/wal.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

std::string TempPath(const char* name) {
  auto dir = std::filesystem::temp_directory_path() / "keel_tests";
  std::filesystem::create_directories(dir);
  auto path = dir / (std::string(name) + "_" + std::to_string(::getpid()));
  std::filesystem::remove(path);
  std::filesystem::remove(std::string(path) + ".wal");
  return path.string();
}

keel::LogRecord Begin(keel::Wal& wal, keel::TxnId txn) {
  keel::LogRecord record;
  record.type = keel::LogType::Begin;
  record.txn = txn;
  record.lsn = wal.next_lsn();
  wal.Append(record);
  return record;
}

}  // namespace

TEST(Wal, RedoCommittedPageThatNeverReachedDisk) {
  const std::string path = TempPath("redo");
  keel::PageId id = 0;
  {
    keel::DiskManager disk(path, false);
    id = disk.Allocate();
    char before[keel::kPageSize];
    disk.Read(id, before);
    keel::Wal wal(path + ".wal", false);
    auto begin = Begin(wal, 1);
    char after[keel::kPageSize];
    std::memcpy(after, before, keel::kPageSize);
    after[80] = 'K';
    keel::LogRecord update;
    update.type = keel::LogType::PageWrite;
    update.txn = 1;
    update.prev = begin.lsn;
    update.lsn = wal.next_lsn();
    update.page = id;
    keel::PageSetLsn(after, update.lsn);
    keel::SealPage(after);
    update.before.assign(before, before + keel::kPageSize);
    update.after.assign(after, after + keel::kPageSize);
    wal.Append(update);
    keel::LogRecord commit;
    commit.type = keel::LogType::Commit;
    commit.txn = 1;
    commit.prev = update.lsn;
    commit.lsn = wal.next_lsn();
    wal.Append(commit);
    wal.Flush();
  }
  keel::DiskManager disk(path, false);
  keel::Wal wal(path + ".wal", false);
  keel::Recover(disk, wal);
  char got[keel::kPageSize];
  disk.Read(id, got);
  EXPECT_EQ(got[80], 'K');
  EXPECT_TRUE(keel::PageChecksumOk(got));
  std::filesystem::remove(path);
  std::filesystem::remove(path + ".wal");
}

TEST(Wal, UndoUncommittedPageThatWasStolen) {
  const std::string path = TempPath("undo");
  keel::PageId id = 0;
  {
    keel::DiskManager disk(path, false);
    id = disk.Allocate();
    char before[keel::kPageSize];
    disk.Read(id, before);
    EXPECT_NE(before[90], 'Z');
    keel::Wal wal(path + ".wal", false);
    auto begin = Begin(wal, 7);
    char after[keel::kPageSize];
    std::memcpy(after, before, keel::kPageSize);
    after[90] = 'Z';
    keel::LogRecord update;
    update.type = keel::LogType::PageWrite;
    update.txn = 7;
    update.prev = begin.lsn;
    update.lsn = wal.next_lsn();
    update.page = id;
    keel::PageSetLsn(after, update.lsn);
    keel::SealPage(after);
    update.before.assign(before, before + keel::kPageSize);
    update.after.assign(after, after + keel::kPageSize);
    wal.Append(update);
    wal.Flush();
    disk.Write(id, after);
  }
  keel::DiskManager disk(path, false);
  keel::Wal wal(path + ".wal", false);
  keel::Recover(disk, wal);
  char got[keel::kPageSize];
  disk.Read(id, got);
  EXPECT_NE(got[90], 'Z');
  EXPECT_TRUE(keel::PageChecksumOk(got));
  std::filesystem::remove(path);
  std::filesystem::remove(path + ".wal");
}

TEST(Wal, TornTailIsIgnored) {
  const std::string path = TempPath("torn");
  keel::PageId id = 0;
  {
    keel::DiskManager disk(path, false);
    id = disk.Allocate();
    char before[keel::kPageSize];
    disk.Read(id, before);
    keel::Wal wal(path + ".wal", false);
    auto begin = Begin(wal, 1);
    char after[keel::kPageSize];
    std::memcpy(after, before, keel::kPageSize);
    after[70] = 'T';
    keel::LogRecord update;
    update.type = keel::LogType::PageWrite;
    update.txn = 1;
    update.prev = begin.lsn;
    update.lsn = wal.next_lsn();
    update.page = id;
    keel::PageSetLsn(after, update.lsn);
    keel::SealPage(after);
    update.before.assign(before, before + keel::kPageSize);
    update.after.assign(after, after + keel::kPageSize);
    wal.Append(update);
    keel::LogRecord commit;
    commit.type = keel::LogType::Commit;
    commit.txn = 1;
    commit.prev = update.lsn;
    commit.lsn = wal.next_lsn();
    wal.Append(commit);
    wal.Flush();
  }
  {
    std::ofstream out(path + ".wal", std::ios::binary | std::ios::app);
    const char junk[24] = {1, 2, 3, 4, 5};
    out.write(junk, sizeof(junk));
  }
  keel::DiskManager disk(path, false);
  keel::Wal wal(path + ".wal", false);
  keel::Recover(disk, wal);
  char got[keel::kPageSize];
  disk.Read(id, got);
  EXPECT_EQ(got[70], 'T');
  std::filesystem::remove(path);
  std::filesystem::remove(path + ".wal");
}
