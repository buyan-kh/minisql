#pragma once

#include "keel/page.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace keel {

// Inserts a record, reusing a dead slot when one exists so RIDs of live
// rows stay put. Returns nullopt when the record cannot fit even after
// compaction. Does not seal the page; the caller logs and seals.
std::optional<uint16_t> SlotInsert(char* page, const void* data, uint16_t len);

// Pointer is into `page` and is invalidated by the next mutation.
std::optional<std::pair<const char*, uint16_t>> SlotGet(const char* page, uint16_t slot);

bool SlotDelete(char* page, uint16_t slot);

// Packs live records toward the header. Slot indexes do not change.
void SlotCompact(char* page);

}  // namespace keel
