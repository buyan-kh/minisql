#include "keel/catalog.hpp"

#include "keel/buf.hpp"
#include "keel/error.hpp"

#include <cstring>

namespace keel {
namespace {

constexpr std::size_t kChunkCap = kPageSize - kPageHeaderSize - 4;

}  // namespace

TableInfo* Catalog::FindTable(std::string_view name) {
  for (auto& table : tables) {
    if (table.name == name) {
      return &table;
    }
  }
  return nullptr;
}

const TableInfo* Catalog::FindTable(std::string_view name) const {
  for (const auto& table : tables) {
    if (table.name == name) {
      return &table;
    }
  }
  return nullptr;
}

IndexInfo* Catalog::FindIndex(std::string_view name) {
  for (auto& index : indexes) {
    if (index.name == name) {
      return &index;
    }
  }
  return nullptr;
}

const IndexInfo* Catalog::FindIndex(std::string_view name) const {
  for (const auto& index : indexes) {
    if (index.name == name) {
      return &index;
    }
  }
  return nullptr;
}

std::string CatalogToBlob(const Catalog& catalog) {
  Buf out;
  out.u32(catalog.next_table_id);
  out.u32(catalog.next_index_id);
  out.u32(static_cast<uint32_t>(catalog.tables.size()));
  for (const auto& table : catalog.tables) {
    out.u32(table.id);
    out.str(table.name);
    out.u32(table.first);
    out.u32(table.last);
    out.u64(table.rows);
    out.u16(static_cast<uint16_t>(table.schema.size()));
    for (const auto& column : table.schema) {
      out.str(column.name);
      out.u8(static_cast<uint8_t>(column.type));
      out.u32(column.varchar_len);
      uint8_t flags = 0;
      if (column.primary_key) {
        flags |= 1;
      }
      if (column.not_null) {
        flags |= 2;
      }
      out.u8(flags);
    }
  }
  out.u32(static_cast<uint32_t>(catalog.indexes.size()));
  for (const auto& index : catalog.indexes) {
    out.u32(index.id);
    out.str(index.name);
    out.u32(index.table_id);
    out.u8(index.unique ? 1 : 0);
    out.u32(index.root);
    out.u16(static_cast<uint16_t>(index.columns.size()));
    for (uint16_t column : index.columns) {
      out.u16(column);
    }
  }
  return out.release();
}

Catalog CatalogFromBlob(std::string_view blob) {
  Reader in(blob);
  Catalog catalog;
  catalog.next_table_id = in.u32();
  catalog.next_index_id = in.u32();
  const uint32_t ntables = in.u32();
  for (uint32_t i = 0; i < ntables; ++i) {
    TableInfo table;
    table.id = in.u32();
    table.name = in.str();
    table.first = in.u32();
    table.last = in.u32();
    table.rows = in.u64();
    const uint16_t ncols = in.u16();
    for (uint16_t c = 0; c < ncols; ++c) {
      Column column;
      column.name = in.str();
      column.type = static_cast<Type>(in.u8());
      column.varchar_len = in.u32();
      const uint8_t flags = in.u8();
      column.primary_key = (flags & 1) != 0;
      column.not_null = (flags & 2) != 0;
      table.schema.push_back(std::move(column));
    }
    catalog.tables.push_back(std::move(table));
  }
  const uint32_t nindexes = in.u32();
  for (uint32_t i = 0; i < nindexes; ++i) {
    IndexInfo index;
    index.id = in.u32();
    index.name = in.str();
    index.table_id = in.u32();
    index.unique = in.u8() != 0;
    index.root = in.u32();
    const uint16_t ncols = in.u16();
    for (uint16_t c = 0; c < ncols; ++c) {
      index.columns.push_back(in.u16());
    }
    catalog.indexes.push_back(std::move(index));
  }
  if (!in.empty()) {
    throw Error("catalog blob has trailing bytes");
  }
  return catalog;
}

void SaveCatalog(Store& store, const Catalog& catalog) {
  const std::string blob = CatalogToBlob(catalog);
  PageId cur = 1;
  std::size_t off = 0;
  bool more = true;
  while (more) {
    const std::size_t n = std::min(kChunkCap, blob.size() - off);
    PageGuard page = store.Fetch(cur);
    if (PageGetType(page.data()) != PageType::Catalog && PageGetId(page.data()) == 1) {
      throw Error("page 1 is not a catalog page");
    }
    const bool remains = off + n < blob.size();
    PageId link = kInvalidPage;
    if (remains) {
      link = PageGetNext(page.data());
      if (link == kInvalidPage) {
        PageGuard born = store.NewPage(PageType::Catalog);
        link = born.id();
      }
    }
    store.Update(page, [&](char* raw) {
      const PageId id = PageGetId(raw);
      const LSN lsn = PageGetLsn(raw);
      std::memset(raw, 0, kPageSize);
      PageSetId(raw, id);
      PageSetType(raw, PageType::Catalog);
      PageSetLsn(raw, lsn);
      PageSetNext(raw, link);
      PageSetExtra(raw, kInvalidPage);
      WriteU32(raw + kPageHeaderSize, static_cast<uint32_t>(n));
      if (n > 0) {
        std::memcpy(raw + kPageHeaderSize + 4, blob.data() + off, n);
      }
    });
    off += n;
    more = remains;
    cur = link;
    if (!more) {
      break;
    }
  }
}

Catalog LoadCatalog(Store& store) {
  std::string blob;
  PageId cur = 1;
  std::size_t hops = 0;
  while (cur != kInvalidPage) {
    if (++hops > 10000) {
      throw Error("catalog page chain is cyclic");
    }
    PageGuard page = store.Fetch(cur);
    if (PageGetType(page.data()) != PageType::Catalog) {
      throw Error("catalog chain contains a non-catalog page");
    }
    const uint32_t n = ReadU32(page.data() + kPageHeaderSize);
    if (kPageHeaderSize + 4 + n > kPageSize) {
      throw Error("catalog chunk length is corrupt");
    }
    blob.append(page.data() + kPageHeaderSize + 4, n);
    cur = PageGetNext(page.data());
  }
  return CatalogFromBlob(blob);
}

}  // namespace keel
