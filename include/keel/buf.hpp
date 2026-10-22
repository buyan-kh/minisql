#pragma once

#include "keel/error.hpp"
#include "keel/page.hpp"

#include <cstring>
#include <string>
#include <string_view>

namespace keel {

class Buf {
 public:
  void u8(uint8_t v) { data_.push_back(static_cast<char>(v)); }
  void u16(uint16_t v) {
    char b[2];
    WriteU16(b, v);
    data_.append(b, 2);
  }
  void u32(uint32_t v) {
    char b[4];
    WriteU32(b, v);
    data_.append(b, 4);
  }
  void u64(uint64_t v) {
    char b[8];
    WriteU64(b, v);
    data_.append(b, 8);
  }
  void bytes(const void* p, std::size_t n) {
    data_.append(static_cast<const char*>(p), n);
  }
  void str(std::string_view s) {
    if (s.size() > 65535) {
      throw Error("string longer than 65535 bytes");
    }
    u16(static_cast<uint16_t>(s.size()));
    data_.append(s);
  }
  const std::string& data() const { return data_; }
  std::string release() { return std::move(data_); }

 private:
  std::string data_;
};

class Reader {
 public:
  Reader(const char* p, std::size_t n) : p_(p), end_(p + n) {}
  explicit Reader(std::string_view s) : Reader(s.data(), s.size()) {}

  std::size_t remaining() const { return static_cast<std::size_t>(end_ - p_); }
  bool empty() const { return p_ == end_; }

  uint8_t u8() {
    need(1);
    return ReadU8(p_++);
  }
  uint16_t u16() {
    need(2);
    uint16_t v = ReadU16(p_);
    p_ += 2;
    return v;
  }
  uint32_t u32() {
    need(4);
    uint32_t v = ReadU32(p_);
    p_ += 4;
    return v;
  }
  uint64_t u64() {
    need(8);
    uint64_t v = ReadU64(p_);
    p_ += 8;
    return v;
  }
  std::string str() {
    uint16_t n = u16();
    need(n);
    std::string s(p_, p_ + n);
    p_ += n;
    return s;
  }
  std::string bytes(std::size_t n) {
    need(n);
    std::string s(p_, p_ + n);
    p_ += n;
    return s;
  }

 private:
  void need(std::size_t n) {
    if (remaining() < n) {
      throw Error("truncated catalog or tuple");
    }
  }
  const char* p_;
  const char* end_;
};

}  // namespace keel
