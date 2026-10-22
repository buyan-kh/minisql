#pragma once

#include <cstddef>
#include <cstdint>

namespace keel {

uint32_t Crc32(const void* data, std::size_t len);

}  // namespace keel
