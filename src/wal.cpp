#include "keel/wal.hpp"

#include "keel/error.hpp"
#include "keel/page.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <map>
#include <queue>

namespace keel {
namespace {

constexpr std::size_t kHeaderBytes = 64;
constexpr char kWalMagic[8] = {'K', 'E', 'E', 'L', 'W', 'A', 'L', '1'};

void Put(std::vector<char>& out, uint32_t v) {
  char b[4];
  WriteU32(b, v);
  out.insert(out.end(), b, b + 4);
}
void Put64(std::vector<char>& out, uint64_t v) {
  char b[8];
  WriteU64(b, v);
  out.insert(out.end(), b, b + 8);
}

std::vector<char> Serialize(const LogRecord& record) {
  std::vector<char> body;
  Put64(body, record.lsn);
  Put64(body, record.txn);
  Put64(body, record.prev);
  body.push_back(static_cast<char>(record.type));
  switch (record.type) {
    case LogType::Begin:
    case LogType::Commit:
    case LogType::Abort:
    case LogType::Shutdown:
      break;
    case LogType::PageWrite: {
      if (record.before.size() != kPageSize || record.after.size() != kPageSize) {
        throw Error("page log record is not a full page image");
      }
      Put(body, record.page);
      body.insert(body.end(), record.before.begin(), record.before.end());
      body.insert(body.end(), record.after.begin(), record.after.end());
      break;
    }
    case LogType::Compensation: {
      if (record.after.size() != kPageSize) {
        throw Error("compensation record is missing its page image");
      }
      Put(body, record.page);
      Put64(body, record.undo_next);
      body.insert(body.end(), record.after.begin(), record.after.end());
      break;
    }
    case LogType::Checkpoint: {
      Put(body, static_cast<uint32_t>(record.active.size()));
      for (const auto& [txn, lsn] : record.active) {
        Put64(body, txn);
        Put64(body, lsn);
      }
      break;
    }
  }
  const uint32_t total = static_cast<uint32_t>(8 + body.size());
  std::vector<char> out;
  out.reserve(total);
  Put(out, total);
  Put(out, Crc32(body.data(), body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

bool Parse(const char* raw, std::size_t available, LogRecord& out) {
  if (available < 8) {
    return false;
  }
  const uint32_t total = ReadU32(raw);
  if (total < 33 || total > 64 * 1024) {
    return false;
  }
  if (available < total) {
    return false;
  }
  const uint32_t crc = ReadU32(raw + 4);
  if (Crc32(raw + 8, total - 8) != crc) {
    return false;
  }
  const char* body = raw + 8;
  const char* end = raw + total;
  auto take64 = [&](const char*& p) {
    uint64_t v = ReadU64(p);
    p += 8;
    return v;
  };
  auto take32 = [&](const char*& p) {
    uint32_t v = ReadU32(p);
    p += 4;
    return v;
  };
  const char* p = body;
  out = LogRecord{};
  out.size = total;
  out.lsn = take64(p);
  out.txn = take64(p);
  out.prev = take64(p);
  out.type = static_cast<LogType>(static_cast<uint8_t>(*p));
  ++p;
  switch (out.type) {
    case LogType::Begin:
    case LogType::Commit:
    case LogType::Abort:
    case LogType::Shutdown:
      break;
    case LogType::PageWrite: {
      out.page = take32(p);
      if (static_cast<std::size_t>(end - p) != kPageSize * 2) {
        return false;
      }
      out.before.assign(p, p + kPageSize);
      p += kPageSize;
      out.after.assign(p, p + kPageSize);
      p += kPageSize;
      break;
    }
    case LogType::Compensation: {
      out.page = take32(p);
      out.undo_next = take64(p);
      if (static_cast<std::size_t>(end - p) != kPageSize) {
        return false;
      }
      out.after.assign(p, p + kPageSize);
      p += kPageSize;
      break;
    }
    case LogType::Checkpoint: {
      const uint32_t n = take32(p);
      if (static_cast<std::size_t>(end - p) != static_cast<std::size_t>(n) * 16) {
        return false;
      }
      for (uint32_t i = 0; i < n; ++i) {
        const TxnId txn = take64(p);
        const LSN lsn = take64(p);
        out.active.emplace_back(txn, lsn);
      }
      break;
    }
    default:
      return false;
  }
  return p == end;
}

LSN PageLsnOrZero(DiskManager& disk, PageId id, char* scratch) {
  if (!disk.TryRead(id, scratch) || !PageChecksumOk(scratch)) {
    return 0;
  }
  return PageGetLsn(scratch);
}

}  // namespace

Wal::Wal(std::string path, bool sync) : path_(std::move(path)), sync_(sync) {
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) {
    throw Error("failed to open write-ahead log " + path_);
  }
  const off_t end = ::lseek(fd_, 0, SEEK_END);
  if (end < 0) {
    throw Error("failed to stat write-ahead log");
  }
  if (end == 0) {
    end_ = kHeaderBytes;
    limit_ = kHeaderBytes;
    flushed_ = 0;
    WriteHeader();
    Flush();
    return;
  }
  std::vector<char> header(kHeaderBytes);
  if (::pread(fd_, header.data(), kHeaderBytes, 0) != static_cast<ssize_t>(kHeaderBytes)) {
    throw Error("write-ahead log header is truncated");
  }
  if (std::memcmp(header.data(), kWalMagic, 8) != 0) {
    throw Error("write-ahead log has a bad magic number");
  }
  char crc_buf[kHeaderBytes];
  std::memcpy(crc_buf, header.data(), kHeaderBytes);
  WriteU32(crc_buf + 8, 0);
  if (Crc32(crc_buf, kHeaderBytes) != ReadU32(header.data() + 8)) {
    checkpoint_ = 0;
  } else {
    checkpoint_ = ReadU64(header.data() + 16);
    generation_ = ReadU64(header.data() + 24);
  }
  limit_ = static_cast<LSN>(end);
  end_ = kHeaderBytes;
  while (end_ < limit_) {
    LogRecord record;
    if (!TryRead(end_, record) || record.lsn != end_) {
      break;
    }
    end_ = record.lsn + record.size;
  }
  if (limit_ != end_) {
    if (::ftruncate(fd_, static_cast<off_t>(end_)) != 0) {
      throw Error("failed to truncate a torn write-ahead log tail");
    }
    limit_ = end_;
  }
  flushed_ = end_;
}

Wal::~Wal() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void Wal::WriteHeader() {
  char header[kHeaderBytes];
  std::memset(header, 0, sizeof(header));
  std::memcpy(header, kWalMagic, 8);
  WriteU64(header + 16, checkpoint_);
  WriteU64(header + 24, generation_);
  WriteU32(header + 8, Crc32(header, kHeaderBytes));
  if (::pwrite(fd_, header, kHeaderBytes, 0) != static_cast<ssize_t>(kHeaderBytes)) {
    throw Error("failed to write the write-ahead log header");
  }
}

LSN Wal::Append(LogRecord record) {
  if (record.lsn == 0) {
    record.lsn = end_;
  }
  if (record.lsn != end_) {
    throw Error("write-ahead log append is not contiguous");
  }
  const std::vector<char> bytes = Serialize(record);
  std::size_t put = 0;
  while (put < bytes.size()) {
    const ssize_t n = ::pwrite(fd_, bytes.data() + put, bytes.size() - put,
                               static_cast<off_t>(end_ + put));
    if (n < 0) {
      throw Error("failed to append to the write-ahead log");
    }
    put += static_cast<std::size_t>(n);
  }
  end_ += bytes.size();
  limit_ = end_;
  return record.lsn;
}

void Wal::Flush() {
  if (end_ == flushed_) {
    return;
  }
  if (sync_ && ::fsync(fd_) != 0) {
    throw Error("failed to fsync the write-ahead log");
  }
  flushed_ = end_;
}

void Wal::FlushThrough(LSN lsn) {
  if (lsn > flushed_) {
    Flush();
  }
}

void Wal::SetCheckpoint(LSN lsn) {
  checkpoint_ = lsn;
  ++generation_;
  WriteHeader();
  Flush();
}

bool Wal::TryRead(LSN lsn, LogRecord& out) const {
  if (lsn < kHeaderBytes || lsn >= limit_) {
    return false;
  }
  char prefix[8];
  if (::pread(fd_, prefix, 8, static_cast<off_t>(lsn)) != 8) {
    return false;
  }
  const uint32_t total = ReadU32(prefix);
  if (total < 33 || total > 64 * 1024 || lsn + total > limit_) {
    return false;
  }
  std::vector<char> raw(total);
  std::size_t got = 0;
  while (got < total) {
    const ssize_t n = ::pread(fd_, raw.data() + got, total - got, static_cast<off_t>(lsn + got));
    if (n <= 0) {
      return false;
    }
    got += static_cast<std::size_t>(n);
  }
  if (!Parse(raw.data(), raw.size(), out) || out.lsn != lsn) {
    return false;
  }
  return true;
}

void Wal::ForEach(const std::function<void(const LogRecord&)>& visit) const {
  LSN cursor = kHeaderBytes;
  while (cursor < end_) {
    LogRecord record;
    if (!TryRead(cursor, record)) {
      break;
    }
    visit(record);
    cursor = record.lsn + record.size;
  }
}

LogRecord Wal::ReadAt(LSN lsn) const {
  LogRecord record;
  if (!TryRead(lsn, record)) {
    throw Error("missing write-ahead log record " + std::to_string(lsn));
  }
  return record;
}

void UndoTransaction(Wal& wal, TxnId txn, LSN last_lsn,
                     const std::function<void(PageId page, const char* image)>& install) {
  LSN cursor = last_lsn;
  while (cursor != 0) {
    const LogRecord record = wal.ReadAt(cursor);
    if (record.txn != txn && record.type != LogType::Begin) {
      throw Error("undo walked onto another transaction");
    }
    if (record.type == LogType::PageWrite) {
      if (record.prev >= cursor) {
        throw Error("undo chain does not move backward");
      }
      std::vector<char> image = record.before;
      const LSN clr_lsn = wal.next_lsn();
      PageSetLsn(image.data(), clr_lsn);
      SealPage(image.data());
      LogRecord clr;
      clr.lsn = clr_lsn;
      clr.txn = txn;
      clr.prev = cursor;
      clr.type = LogType::Compensation;
      clr.page = record.page;
      clr.undo_next = record.prev;
      clr.after = image;
      wal.Append(clr);
      install(record.page, image.data());
      cursor = record.prev;
    } else if (record.type == LogType::Compensation) {
      cursor = record.undo_next;
    } else {
      cursor = 0;
    }
  }
  LogRecord abort;
  abort.lsn = wal.next_lsn();
  abort.txn = txn;
  abort.prev = last_lsn;
  abort.type = LogType::Abort;
  wal.Append(abort);
  wal.Flush();
}

void Recover(DiskManager& disk, Wal& wal) {
  std::map<TxnId, LSN> active;
  const LSN checkpoint = wal.checkpoint_lsn();
  bool saw_checkpoint = checkpoint == 0;
  wal.ForEach([&](const LogRecord& record) {
    if (!saw_checkpoint) {
      if (record.lsn < checkpoint) {
        return;
      }
      if (record.lsn == checkpoint && record.type == LogType::Checkpoint) {
        for (const auto& [txn, lsn] : record.active) {
          if (lsn != 0) {
            active[txn] = lsn;
          }
        }
        saw_checkpoint = true;
        return;
      }
      saw_checkpoint = true;
    }
    if (record.lsn <= checkpoint) {
      return;
    }
    switch (record.type) {
      case LogType::Begin:
      case LogType::PageWrite:
      case LogType::Compensation:
        active[record.txn] = record.lsn;
        break;
      case LogType::Commit:
      case LogType::Abort:
        active.erase(record.txn);
        break;
      default:
        break;
    }
  });

  wal.ForEach([&](const LogRecord& record) {
    if (record.lsn <= checkpoint) {
      return;
    }
    if (record.type != LogType::PageWrite && record.type != LogType::Compensation) {
      return;
    }
    char scratch[kPageSize];
    const LSN have = PageLsnOrZero(disk, record.page, scratch);
    if (have < record.lsn) {
      disk.Write(record.page, record.after.data());
    }
  });

  using Item = std::pair<LSN, TxnId>;
  std::priority_queue<Item> pending;
  for (const auto& [txn, lsn] : active) {
    if (lsn != 0) {
      pending.push({lsn, txn});
    }
  }
  std::map<PageId, std::vector<char>> undone;
  while (!pending.empty()) {
    const Item item = pending.top();
    pending.pop();
    const LogRecord record = wal.ReadAt(item.first);
    if (record.type == LogType::PageWrite) {
      std::vector<char> image = record.before;
      const LSN clr_lsn = wal.next_lsn();
      PageSetLsn(image.data(), clr_lsn);
      SealPage(image.data());
      LogRecord clr;
      clr.lsn = clr_lsn;
      clr.txn = record.txn;
      clr.prev = record.lsn;
      clr.type = LogType::Compensation;
      clr.page = record.page;
      clr.undo_next = record.prev;
      clr.after = image;
      wal.Append(clr);
      undone[record.page] = std::move(image);
      if (record.prev != 0) {
        pending.push({record.prev, record.txn});
      } else {
        LogRecord abort;
        abort.lsn = wal.next_lsn();
        abort.txn = record.txn;
        abort.prev = clr.lsn;
        abort.type = LogType::Abort;
        wal.Append(abort);
      }
    } else if (record.type == LogType::Compensation) {
      if (record.undo_next != 0) {
        pending.push({record.undo_next, record.txn});
      }
    } else if (record.type == LogType::Begin) {
      LogRecord abort;
      abort.lsn = wal.next_lsn();
      abort.txn = record.txn;
      abort.prev = record.lsn;
      abort.type = LogType::Abort;
      wal.Append(abort);
    }
  }
  wal.Flush();
  for (const auto& [page, image] : undone) {
    disk.Write(page, image.data());
  }
  disk.Sync();
}

}  // namespace keel
