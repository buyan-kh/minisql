#pragma once

#include "keel/config.hpp"
#include "keel/disk_manager.hpp"

#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace keel {

enum class EvictPolicy { Clock, Lru };

struct BufferStats {
  uint64_t hits{0};
  uint64_t misses{0};
  uint64_t writes{0};
  uint64_t evictions{0};
};

class BufferPool;

// RAII pin. Destroying the guard unpins the frame. Move-only, so a page
// cannot be forgotten in an early return.
class PageGuard {
 public:
  PageGuard() = default;
  ~PageGuard();
  PageGuard(PageGuard&& other) noexcept;
  PageGuard& operator=(PageGuard&& other) noexcept;
  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;

  explicit operator bool() const { return pool_ != nullptr; }
  char* data() const { return data_; }
  PageId id() const { return id_; }
  void MarkDirty();

 private:
  friend class BufferPool;
  PageGuard(BufferPool* pool, std::size_t frame, char* data, PageId id);
  void Release();

  BufferPool* pool_{nullptr};
  std::size_t frame_{0};
  char* data_{nullptr};
  PageId id_{kInvalidPage};
};

class BufferPool {
 public:
  // `flush_log` is invoked with a page LSN before that dirty page is written.
  // It must not re-enter the pool. This is the WAL rule: log before data.
  BufferPool(DiskManager& disk, std::size_t frames, EvictPolicy policy,
             std::function<void(LSN)> flush_log);
  ~BufferPool();

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  PageGuard Fetch(PageId id);
  // Extends the file with a sealed free page and returns its id. Not pinned.
  PageId Allocate();
  void FlushAll();
  // Drops dirty frames without writing them. Used to simulate a crash.
  void Discard();
  BufferStats Stats() const;
  void ResetStats();
  std::size_t capacity() const { return frames_.size(); }

 private:
  friend class PageGuard;

  struct Frame {
    PageId id{kInvalidPage};
    int pin{0};
    bool valid{false};
    bool dirty{false};
    bool ref{false};
    std::vector<char> data;
  };

  PageGuard PinLocked(std::size_t idx);
  void Unpin(std::size_t frame);
  void Mark(std::size_t frame);
  std::size_t VictimLocked();
  void EvictLocked(std::size_t idx);
  void FlushLocked(Frame& frame);
  void TouchLocked(std::size_t idx);
  void UntouchLocked(std::size_t idx);

  DiskManager& disk_;
  EvictPolicy policy_;
  std::function<void(LSN)> flush_log_;
  mutable std::mutex mu_;
  std::vector<Frame> frames_;
  std::unordered_map<PageId, std::size_t> table_;
  std::vector<std::size_t> free_;
  std::size_t clock_{0};
  std::list<std::size_t> lru_;
  std::vector<std::list<std::size_t>::iterator> lru_it_;
  mutable BufferStats stats_;
};

}  // namespace keel
