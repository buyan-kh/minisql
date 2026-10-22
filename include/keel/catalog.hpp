#pragma once

#include "keel/store.hpp"
#include "keel/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace keel {

struct TableInfo {
  uint32_t id{0};
  std::string name;
  Schema schema;
  PageId first{kInvalidPage};
  PageId last{kInvalidPage};
  uint64_t rows{0};
};

struct IndexInfo {
  uint32_t id{0};
  std::string name;
  uint32_t table_id{0};
  std::vector<uint16_t> columns;
  bool unique{false};
  PageId root{kInvalidPage};
};

struct Catalog {
  uint32_t next_table_id{1};
  uint32_t next_index_id{1};
  std::vector<TableInfo> tables;
  std::vector<IndexInfo> indexes;

  TableInfo* FindTable(std::string_view name);
  const TableInfo* FindTable(std::string_view name) const;
  IndexInfo* FindIndex(std::string_view name);
  const IndexInfo* FindIndex(std::string_view name) const;
};

std::string CatalogToBlob(const Catalog& catalog);
Catalog CatalogFromBlob(std::string_view blob);
void SaveCatalog(Store& store, const Catalog& catalog);
Catalog LoadCatalog(Store& store);

}  // namespace keel
