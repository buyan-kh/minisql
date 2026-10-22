#include "keel/btree.hpp"

#include "keel/buf.hpp"
#include "keel/error.hpp"
#include "keel/slotted.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace keel {
namespace {

constexpr std::size_t kMaxIndexKey = 1024;

struct LeafEntry {
  std::string key;
  RID rid;
};

struct Internal {
  std::vector<std::string> keys;
  std::vector<PageId> children;
};

std::string EncodeLeaf(const LeafEntry& entry) {
  Buf out;
  if (entry.key.size() > 65535) {
    throw Error("index key too large");
  }
  out.u16(static_cast<uint16_t>(entry.key.size()));
  out.bytes(entry.key.data(), entry.key.size());
  out.u32(entry.rid.page);
  out.u16(entry.rid.slot);
  return out.release();
}

LeafEntry DecodeLeaf(const char* data, uint16_t len) {
  Reader in(data, len);
  const uint16_t key_len = in.u16();
  LeafEntry entry;
  entry.key = in.bytes(key_len);
  entry.rid.page = in.u32();
  entry.rid.slot = in.u16();
  if (!in.empty()) {
    throw Error("corrupt leaf entry");
  }
  return entry;
}

std::string EncodeInternal(const std::string& key, PageId child) {
  Buf out;
  out.u16(static_cast<uint16_t>(key.size()));
  out.bytes(key.data(), key.size());
  out.u32(child);
  return out.release();
}

std::pair<std::string, PageId> DecodeInternal(const char* data, uint16_t len) {
  Reader in(data, len);
  const uint16_t key_len = in.u16();
  std::string key = in.bytes(key_len);
  const PageId child = in.u32();
  if (!in.empty()) {
    throw Error("corrupt internal entry");
  }
  return {std::move(key), child};
}

bool PayloadsFit(const std::vector<std::string>& payloads) {
  std::size_t bytes = 0;
  for (const auto& payload : payloads) {
    bytes += payload.size() + 4;
  }
  return bytes <= kPageSize - kPageHeaderSize;
}

std::vector<LeafEntry> ReadLeaf(const char* page) {
  if (PageGetType(page) != PageType::Leaf) {
    throw Error("expected a B+tree leaf");
  }
  std::vector<LeafEntry> entries;
  const uint16_t n = PageGetSlots(page);
  entries.reserve(n);
  for (uint16_t i = 0; i < n; ++i) {
    auto rec = SlotGet(page, i);
    if (!rec) {
      throw Error("hole in a B+tree leaf");
    }
    entries.push_back(DecodeLeaf(rec->first, rec->second));
  }
  return entries;
}

Internal ReadInternal(const char* page) {
  if (PageGetType(page) != PageType::Internal) {
    throw Error("expected a B+tree internal node");
  }
  Internal node;
  const uint16_t n = PageGetSlots(page);
  for (uint16_t i = 0; i < n; ++i) {
    auto rec = SlotGet(page, i);
    if (!rec) {
      throw Error("hole in a B+tree internal node");
    }
    auto [key, child] = DecodeInternal(rec->first, rec->second);
    node.keys.push_back(std::move(key));
    node.children.push_back(child);
  }
  node.children.push_back(PageGetExtra(page));
  if (node.children.back() == kInvalidPage) {
    throw Error("internal node is missing its rightmost child");
  }
  return node;
}

void WritePayloads(char* page, PageType type, const std::vector<std::string>& payloads, PageId next,
                   PageId extra) {
  if (!PayloadsFit(payloads)) {
    throw Error("B+tree node does not fit in a page");
  }
  const PageId id = PageGetId(page);
  const LSN lsn = PageGetLsn(page);
  std::memset(page, 0, kPageSize);
  PageSetId(page, id);
  PageSetType(page, type);
  PageSetLsn(page, lsn);
  PageSetNext(page, next);
  PageSetExtra(page, extra);
  uint16_t off = static_cast<uint16_t>(kPageHeaderSize);
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    std::memcpy(page + off, payloads[i].data(), payloads[i].size());
    SlotSet(page, static_cast<uint16_t>(i), off, static_cast<uint16_t>(payloads[i].size()));
    off = static_cast<uint16_t>(off + payloads[i].size());
  }
  PageSetSlots(page, static_cast<uint16_t>(payloads.size()));
  PageSetFreeBegin(page, off);
  PageSetFreeEnd(page, static_cast<uint16_t>(kPageSize - payloads.size() * 4));
}

bool LeafFits(const std::vector<LeafEntry>& entries) {
  std::vector<std::string> payloads;
  payloads.reserve(entries.size());
  for (const auto& entry : entries) {
    payloads.push_back(EncodeLeaf(entry));
  }
  return PayloadsFit(payloads);
}

void WriteLeaf(char* page, const std::vector<LeafEntry>& entries, PageId next) {
  std::vector<std::string> payloads;
  payloads.reserve(entries.size());
  for (const auto& entry : entries) {
    payloads.push_back(EncodeLeaf(entry));
  }
  WritePayloads(page, PageType::Leaf, payloads, next, kInvalidPage);
}

bool InternalFits(const Internal& node) {
  std::vector<std::string> payloads;
  for (std::size_t i = 0; i < node.keys.size(); ++i) {
    payloads.push_back(EncodeInternal(node.keys[i], node.children[i]));
  }
  return PayloadsFit(payloads);
}

void WriteInternal(char* page, const Internal& node) {
  if (node.children.size() != node.keys.size() + 1) {
    throw Error("internal node child count is wrong");
  }
  std::vector<std::string> payloads;
  for (std::size_t i = 0; i < node.keys.size(); ++i) {
    payloads.push_back(EncodeInternal(node.keys[i], node.children[i]));
  }
  WritePayloads(page, PageType::Internal, payloads, kInvalidPage, node.children.back());
}

PageId FindChild(const Internal& node, const std::string& key) {
  for (std::size_t i = 0; i < node.keys.size(); ++i) {
    if (CompareBytes(key, node.keys[i]) < 0) {
      return node.children[i];
    }
  }
  return node.children.back();
}

PageId Leftmost(Store& store, PageId root) {
  PageId cur = root;
  for (;;) {
    PageGuard page = store.Fetch(cur);
    if (PageGetType(page.data()) == PageType::Leaf) {
      return cur;
    }
    Internal node = ReadInternal(page.data());
    cur = node.children.front();
  }
}

}  // namespace

BTree::BTree(Store& store, PageId& root) : store_(store), root_(root) {}

PageId BTree::Create(Store& store) {
  PageGuard page = store.NewPage(PageType::Leaf);
  return page.id();
}

void BTree::Insert(const std::string& key, RID rid) {
  if (key.size() > kMaxIndexKey) {
    throw Error("index key exceeds 1024 bytes");
  }
  if (root_ == kInvalidPage) {
    throw Error("B+tree has no root");
  }
  std::vector<PageId> parents;
  PageId leaf_id = root_;
  while (true) {
    PageGuard page = store_.Fetch(leaf_id);
    const PageType type = PageGetType(page.data());
    if (type == PageType::Leaf) {
      break;
    }
    if (type != PageType::Internal) {
      throw Error("corrupt B+tree page");
    }
    const PageId child = FindChild(ReadInternal(page.data()), key);
    parents.push_back(leaf_id);
    leaf_id = child;
  }

  PageGuard leaf = store_.Fetch(leaf_id);
  std::vector<LeafEntry> entries = ReadLeaf(leaf.data());
  const PageId next = PageGetNext(leaf.data());
  LeafEntry created{key, rid};
  auto place = std::lower_bound(entries.begin(), entries.end(), created,
                                [](const LeafEntry& a, const LeafEntry& b) {
                                  return CompareBytes(a.key, b.key) < 0;
                                });
  if (place != entries.end() && place->key == key) {
    throw Error("duplicate key");
  }
  entries.insert(place, std::move(created));
  if (LeafFits(entries)) {
    store_.Update(leaf, [&](char* raw) { WriteLeaf(raw, entries, next); });
    return;
  }

  const std::size_t mid = entries.size() / 2;
  if (mid == 0 || mid == entries.size()) {
    throw Error("a single index entry does not fit in a leaf");
  }
  std::vector<LeafEntry> left(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(mid));
  std::vector<LeafEntry> right(entries.begin() + static_cast<std::ptrdiff_t>(mid), entries.end());
  const std::string separator = right.front().key;
  PageGuard sibling = store_.NewPage(PageType::Leaf);
  const PageId sibling_id = sibling.id();
  store_.Update(sibling, [&](char* raw) { WriteLeaf(raw, right, next); });
  store_.Update(leaf, [&](char* raw) { WriteLeaf(raw, left, sibling_id); });
  leaf = PageGuard();
  sibling = PageGuard();

  PageId left_id = leaf_id;
  std::string sep = separator;
  PageId right_id = sibling_id;
  while (true) {
    if (parents.empty()) {
      PageGuard root = store_.NewPage(PageType::Internal);
      Internal node;
      node.keys = {sep};
      node.children = {left_id, right_id};
      store_.Update(root, [&](char* raw) { WriteInternal(raw, node); });
      root_ = root.id();
      return;
    }
    const PageId parent_id = parents.back();
    parents.pop_back();
    PageGuard parent = store_.Fetch(parent_id);
    Internal node = ReadInternal(parent.data());
    auto found = std::find(node.children.begin(), node.children.end(), left_id);
    if (found == node.children.end()) {
      throw Error("parent does not point at the split child");
    }
    const std::size_t index = static_cast<std::size_t>(found - node.children.begin());
    node.keys.insert(node.keys.begin() + static_cast<std::ptrdiff_t>(index), sep);
    node.children.insert(node.children.begin() + static_cast<std::ptrdiff_t>(index + 1), right_id);
    if (InternalFits(node)) {
      store_.Update(parent, [&](char* raw) { WriteInternal(raw, node); });
      return;
    }
    const std::size_t split_at = node.keys.size() / 2;
    const std::string promoted = node.keys[split_at];
    Internal lower;
    Internal upper;
    lower.keys.assign(node.keys.begin(), node.keys.begin() + static_cast<std::ptrdiff_t>(split_at));
    lower.children.assign(node.children.begin(),
                          node.children.begin() + static_cast<std::ptrdiff_t>(split_at + 1));
    upper.keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(split_at + 1), node.keys.end());
    upper.children.assign(node.children.begin() + static_cast<std::ptrdiff_t>(split_at + 1),
                          node.children.end());
    if (!InternalFits(lower) || !InternalFits(upper)) {
      throw Error("index key does not fit in an internal page");
    }
    PageGuard born = store_.NewPage(PageType::Internal);
    const PageId born_id = born.id();
    store_.Update(born, [&](char* raw) { WriteInternal(raw, upper); });
    store_.Update(parent, [&](char* raw) { WriteInternal(raw, lower); });
    left_id = parent_id;
    right_id = born_id;
    sep = promoted;
  }
}

bool BTree::Remove(const std::string& key) {
  PageId leaf_id = root_;
  while (true) {
    PageGuard page = store_.Fetch(leaf_id);
    if (PageGetType(page.data()) == PageType::Leaf) {
      break;
    }
    leaf_id = FindChild(ReadInternal(page.data()), key);
  }
  PageGuard leaf = store_.Fetch(leaf_id);
  std::vector<LeafEntry> entries = ReadLeaf(leaf.data());
  const PageId next = PageGetNext(leaf.data());
  const auto it = std::find_if(entries.begin(), entries.end(),
                               [&](const LeafEntry& entry) { return entry.key == key; });
  if (it == entries.end()) {
    return false;
  }
  entries.erase(it);
  store_.Update(leaf, [&](char* raw) { WriteLeaf(raw, entries, next); });
  return true;
}

void BTree::Scan(const std::string* start,
                 const std::function<bool(const std::string& key, RID rid)>& visit) const {
  if (root_ == kInvalidPage) {
    return;
  }
  PageId cur = start == nullptr ? Leftmost(store_, root_) : root_;
  if (start != nullptr) {
    while (true) {
      PageGuard page = store_.Fetch(cur);
      if (PageGetType(page.data()) == PageType::Leaf) {
        break;
      }
      cur = FindChild(ReadInternal(page.data()), *start);
    }
  }
  bool checking = start != nullptr;
  while (cur != kInvalidPage) {
    PageGuard page = store_.Fetch(cur);
    const PageId next = PageGetNext(page.data());
    for (const auto& entry : ReadLeaf(page.data())) {
      if (checking && CompareBytes(entry.key, *start) < 0) {
        continue;
      }
      checking = false;
      if (!visit(entry.key, entry.rid)) {
        return;
      }
    }
    cur = next;
  }
}

}  // namespace keel
