#pragma once

#include "keel/store.hpp"
#include "keel/types.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace keel {

RID HeapInsert(Store& store, PageId& first, PageId& last, const uint8_t* data, uint16_t len);
std::optional<std::vector<uint8_t>> HeapRead(Store& store, RID rid);
bool HeapDelete(Store& store, RID rid);
void HeapForEach(Store& store, PageId first,
                 const std::function<bool(RID rid, std::string_view bytes)>& visit);

}  // namespace keel
