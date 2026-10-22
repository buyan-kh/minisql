#include "keel/buffer_pool.hpp"

#include "keel/error.hpp"
#include "keel/page.hpp"

#include <optional>
#include <utility>

namespace keel {

PageGuard::PageGuard(BufferPool* pool, std::size_t frame, char* data, PageId id)
    : pool_(pool), frame_(frame), data_(data), id_(id) {}

PageGuard::~PageGuard() { Release(); }

PageGuard::PageGuard(PageGuard&& other) noexcept { *this = std::move(other); }

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
  if (this != &other) {
    Release();
    pool_ = other.pool_;
    frame_ = other.frame_;
    data_ = other.data_;
    id_ = other.id_;
    other.pool_ = nullptr;
    other.data_ = nullptr;
  }
  return *this;
}

void PageGuard::Release() {
  if (pool_ != nullptr) {
    pool_->Unpin(frame_);
    pool_ = nullptr;
    data_ = nullptr;
  }
}

void PageGuard::MarkDirty() {
  if (pool_ == nullptr) {
    throw Error("mark dirty on an empty page guard");
  }
  pool_->Mark(frame_);
}

BufferPool::BufferPool(DiskManager& disk, std::size_t frames, EvictPolicy policy,
                       std::function<void(LSN)> flush_log)
    : disk_(disk), policy_(policy), flush_log_(std::move(flush_log)) {
  if (frames < 8) {
    throw Error("buffer pool needs at least 8 frames");
  }
  frames_.resize(frames);
  lru_it_.assign(frames, lru_.end());
  for (std::size_t i = 0; i < frames; ++i) {
    frames_[i].data.assign(kPageSize, 0);
    free_.push_back(i);
  }
}

BufferPool::~BufferPool() = default;

PageGuard BufferPool::PinLocked(std::size_t idx) {
  Frame& frame = frames_[idx];
  ++frame.pin;
  return PageGuard(this, idx, frame.data.data(), frame.id);
}

void BufferPool::Unpin(std::size_t frame) {
  std::lock_guard lock(mu_);
  Frame& f = frames_[frame];
  if (f.pin <= 0) {
    throw Error("unpin of a page that is not pinned");
  }
  --f.pin;
  if (f.pin == 0) {
    f.ref = true;
    TouchLocked(frame);
  }
}

void BufferPool::Mark(std::size_t frame) {
  std::lock_guard lock(mu_);
  frames_[frame].dirty = true;
}

void BufferPool::TouchLocked(std::size_t idx) {
  if (policy_ != EvictPolicy::Lru) {
    return;
  }
  if (lru_it_[idx] != lru_.end()) {
    lru_.erase(lru_it_[idx]);
  }
  lru_.push_back(idx);
  lru_it_[idx] = std::prev(lru_.end());
}

void BufferPool::UntouchLocked(std::size_t idx) {
  if (policy_ != EvictPolicy::Lru || lru_it_[idx] == lru_.end()) {
    return;
  }
  lru_.erase(lru_it_[idx]);
  lru_it_[idx] = lru_.end();
}

void BufferPool::FlushLocked(Frame& frame) {
  if (!frame.valid || !frame.dirty) {
    return;
  }
  if (flush_log_) {
    flush_log_(PageGetLsn(frame.data.data()));
  }
  disk_.Write(frame.id, frame.data.data());
  frame.dirty = false;
  ++stats_.writes;
}

void BufferPool::EvictLocked(std::size_t idx) {
  Frame& frame = frames_[idx];
  if (!frame.valid) {
    return;
  }
  if (frame.pin != 0) {
    throw Error("attempted to evict a pinned page");
  }
  FlushLocked(frame);
  table_.erase(frame.id);
  UntouchLocked(idx);
  frame.valid = false;
  frame.dirty = false;
  frame.ref = false;
  frame.id = kInvalidPage;
  ++stats_.evictions;
}

std::size_t BufferPool::VictimLocked() {
  const std::size_t n = frames_.size();
  if (policy_ == EvictPolicy::Lru) {
    for (std::size_t idx : lru_) {
      if (frames_[idx].valid && frames_[idx].pin == 0) {
        return idx;
      }
    }
    throw Error("buffer pool: no evictable frame (all pages pinned)");
  }
  for (std::size_t step = 0; step < n * 2; ++step) {
    const std::size_t idx = clock_;
    clock_ = (clock_ + 1) % n;
    Frame& frame = frames_[idx];
    if (!frame.valid) {
      return idx;
    }
    if (frame.pin > 0) {
      continue;
    }
    if (frame.ref) {
      frame.ref = false;
      continue;
    }
    return idx;
  }
  throw Error("buffer pool: no evictable frame (all pages pinned)");
}

PageGuard BufferPool::Fetch(PageId id) {
  std::lock_guard lock(mu_);
  if (auto it = table_.find(id); it != table_.end()) {
    ++stats_.hits;
    frames_[it->second].ref = true;
    TouchLocked(it->second);
    return PinLocked(it->second);
  }
  ++stats_.misses;
  std::size_t idx;
  if (!free_.empty()) {
    idx = free_.back();
    free_.pop_back();
  } else {
    idx = VictimLocked();
    EvictLocked(idx);
  }
  Frame& frame = frames_[idx];
  disk_.Read(id, frame.data.data());
  if (!PageChecksumOk(frame.data.data())) {
    throw Error("checksum mismatch on page " + std::to_string(id));
  }
  if (PageGetId(frame.data.data()) != id) {
    throw Error("page id mismatch reading page " + std::to_string(id));
  }
  frame.id = id;
  frame.valid = true;
  frame.dirty = false;
  frame.ref = true;
  frame.pin = 0;
  table_[id] = idx;
  TouchLocked(idx);
  return PinLocked(idx);
}

PageId BufferPool::Allocate() { return disk_.Allocate(); }

void BufferPool::FlushAll() {
  std::lock_guard lock(mu_);
  for (auto& frame : frames_) {
    FlushLocked(frame);
  }
  disk_.Sync();
}

void BufferPool::Discard() {
  std::lock_guard lock(mu_);
  for (const auto& frame : frames_) {
    if (frame.pin != 0) {
      throw Error("cannot discard the buffer pool while pages are pinned");
    }
  }
  table_.clear();
  lru_.clear();
  free_.clear();
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    frames_[i].valid = false;
    frames_[i].dirty = false;
    frames_[i].ref = false;
    frames_[i].pin = 0;
    frames_[i].id = kInvalidPage;
    lru_it_[i] = lru_.end();
    free_.push_back(i);
  }
}

BufferStats BufferPool::Stats() const {
  std::lock_guard lock(mu_);
  return stats_;
}

void BufferPool::ResetStats() {
  std::lock_guard lock(mu_);
  stats_ = {};
}

}  // namespace keel
