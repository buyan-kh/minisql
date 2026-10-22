#pragma once

#include "keel/config.hpp"
#include "keel/disk_manager.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace keel {

enum class LogType : uint8_t {
  Begin = 1,
  Commit = 2,
  Abort = 3,
  PageWrite = 4,
  Compensation = 5,
  Checkpoint = 6,
  Shutdown = 7,
};

struct LogRecord {
  LSN lsn{0};
  uint32_t size{0};
  TxnId txn{0};
  LSN prev{0};
  LogType type{LogType::Begin};
  PageId page{kInvalidPage};
  LSN undo_next{0};
  std::vector<char> before;
  std::vector<char> after;
  std::vector<std::pair<TxnId, LSN>> active;
};

// Append-only write-ahead log. The header stores the LSN of the last sharp
// checkpoint. A torn tail (crash mid-append) fails the checksum and is truncated.
class Wal {
 public:
  Wal(std::string path, bool sync);
  ~Wal();

  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  LSN next_lsn() const { return end_; }
  LSN checkpoint_lsn() const { return checkpoint_; }
  LSN Append(LogRecord record);
  void FlushThrough(LSN lsn);
  void Flush();
  void SetCheckpoint(LSN lsn);
  void ForEach(const std::function<void(const LogRecord&)>& visit) const;
  LogRecord ReadAt(LSN lsn) const;

 private:
  void WriteHeader();
  bool TryRead(LSN lsn, LogRecord& out) const;

  std::string path_;
  int fd_{-1};
  bool sync_{true};
  LSN end_{0};
  LSN limit_{0};
  LSN flushed_{0};
  LSN checkpoint_{0};
  uint64_t generation_{1};
};

// Repeat history, then undo every transaction that never committed.
// `install` receives the final image of a page undone in this pass. The log is
// flushed before install returns, so the caller may write the image safely.
void Recover(DiskManager& disk, Wal& wal);

// Undo one transaction, appending compensation records. `install` is invoked
// after each compensation record is appended and before the final flush.
void UndoTransaction(Wal& wal, TxnId txn, LSN last_lsn,
                     const std::function<void(PageId page, const char* image)>& install);

}  // namespace keel
