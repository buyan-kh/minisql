#include "keel/crc32.hpp"

#include <array>
#include <cstdint>

namespace keel {
namespace {

const std::array<uint32_t, 256>& Table() {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int bit = 0; bit < 8; ++bit) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  return table;
}

}  // namespace

uint32_t Crc32(const void* data, std::size_t len) {
  const auto& table = Table();
  uint32_t c = 0xFFFFFFFFu;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (std::size_t i = 0; i < len; ++i) {
    c = table[(c ^ bytes[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

}  // namespace keel
