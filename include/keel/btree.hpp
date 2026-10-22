#pragma once

#include "keel/store.hpp"
#include "keel/types.hpp"

#include <functional>
#include <string>

namespace keel {

// A B+tree of unique binary keys. Duplicate user values are made unique by
// appending the RID, so leaf splits never have to place the same key on both
// sides of a separator. Underfull leaves are left in place on delete; the
// separator invariant still holds because keys only move during splits.
class BTree {
 public:
  BTree(Store& store, PageId& root);

  static PageId Create(Store& store);

  void Insert(const std::string& key, RID rid);
  bool Remove(const std::string& key);

  // Walk keys in order, starting at the first key >= `start` when start is set.
  // Stop early when the callback returns false.
  void Scan(const std::string* start,
            const std::function<bool(const std::string& key, RID rid)>& visit) const;

 private:
  Store& store_;
  PageId& root_;
};

}  // namespace keel
