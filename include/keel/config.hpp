#pragma once

#include <cstddef>
#include <cstdint>

namespace keel {

// 4 KiB matches common filesystem and SSD page sizes, so a buffer-pool
// write is one device block and checksum failures line up with torn writes.
constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kPageHeaderSize = 32;

using PageId = uint32_t;
using LSN = uint64_t;
using TxnId = uint64_t;

constexpr PageId kInvalidPage = 0xFFFFFFFFu;
constexpr LSN kInvalidLsn = 0;

// Every index entry appends this many bytes so duplicate user keys stay unique.
constexpr std::size_t kRidBytes = 6;

}  // namespace keel
