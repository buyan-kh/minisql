#include "keel/executor.hpp"

#include "keel/btree.hpp"
#include "keel/error.hpp"
#include "keel/expr.hpp"
#include "keel/heap.hpp"
#include "keel/page.hpp"
#include "keel/slotted.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace keel {
namespace {

enum class Rel { Before, Equal, After };

Rel Relate(std::string_view key, std::string_view bound) {
  const std::size_t n = std::min(key.size(), bound.size());
  if (n > 0) {
    const int cmp = std::memcmp(key.data(), bound.data(), n);
    if (cmp < 0) {
      return Rel::Before;
    }
    if (cmp > 0) {
      return Rel::After;
    }
  }
  if (key.size() >= bound.size()) {
    return Rel::Equal;
  }
  return Rel::Before;
}

bool PassLow(std::string_view user, const Plan& plan) {
  if (!plan.low_key) {
    return true;
  }
  const Rel rel = Relate(user, *plan.low_key);
  if (plan.low_incl) {
    return rel != Rel::Before;
  }
  return rel == Rel::After;
}

bool PassHigh(std::string_view user, const Plan& plan) {
  if (!plan.high_key) {
    return true;
  }
  const Rel rel = Relate(user, *plan.high_key);
  if (plan.high_incl) {
    return rel != Rel::After;
  }
  return rel == Rel::Before;
}

int CompareNullsLow(const Value& left, const Value& right) {
  if (left.is_null || right.is_null) {
    if (left.is_null && right.is_null) {
      return 0;
    }
    return left.is_null ? -1 : 1;
  }
  return CompareValues(left, right);
}

std::string KeyOf(const Tuple& row, const std::vector<int>& keys) {
  std::vector<Value> values;
  values.reserve(keys.size());
  for (int key : keys) {
    values.push_back(row[static_cast<std::size_t>(key)]);
  }
  return EncodeValues(values);
}

bool KeysNull(const Tuple& row, const std::vector<int>& keys) {
  for (int key : keys) {
    if (row[static_cast<std::size_t>(key)].is_null) {
      return true;
    }
  }
  return false;
}

class SeqScanExec final : public Executor {
 public:
  SeqScanExec(Store& store, const Plan& plan, const TableInfo& table)
      : store_(store), plan_(plan), table_(table) {}

  void Open() override {
    page_ = table_.first;
    slot_ = 0;
    open_ = true;
  }

  bool Next(Tuple& row) override {
    if (!open_) {
      throw Error("scan is not open");
    }
    while (page_ != kInvalidPage) {
      PageGuard guard = store_.Fetch(page_);
      const uint16_t slots = PageGetSlots(guard.data());
      const PageId next = PageGetNext(guard.data());
      while (slot_ < slots) {
        const uint16_t slot = slot_++;
        auto rec = SlotGet(guard.data(), slot);
        if (!rec) {
          continue;
        }
        row = DeserializeTuple(plan_.schema, reinterpret_cast<const uint8_t*>(rec->first), rec->second);
        return true;
      }
      page_ = next;
      slot_ = 0;
    }
    return false;
  }

  void Close() override { open_ = false; }

 private:
  Store& store_;
  const Plan& plan_;
  const TableInfo& table_;
  PageId page_{kInvalidPage};
  uint16_t slot_{0};
  bool open_{false};
};

class IndexScanExec final : public Executor {
 public:
  IndexScanExec(Store& store, const Plan& plan, const IndexInfo& index)
      : store_(store), plan_(plan), index_(index) {}

  void Open() override {
    rids_.clear();
    cursor_ = 0;
    PageId root = index_.root;
    BTree tree(store_, root);
    const std::string* start = plan_.low_key ? &plan_.low_key.value() : nullptr;
    tree.Scan(start, [&](const std::string& key, RID rid) {
      const std::string_view user = UserKeyOf(key);
      if (!PassLow(user, plan_)) {
        return true;
      }
      if (!PassHigh(user, plan_)) {
        return false;
      }
      rids_.push_back(rid);
      return true;
    });
    open_ = true;
  }

  bool Next(Tuple& row) override {
    if (!open_) {
      throw Error("scan is not open");
    }
    while (cursor_ < rids_.size()) {
      const RID rid = rids_[cursor_++];
      auto bytes = HeapRead(store_, rid);
      if (!bytes) {
        continue;
      }
      row = DeserializeTuple(plan_.schema, bytes->data(), bytes->size());
      return true;
    }
    return false;
  }

  void Close() override { open_ = false; }

 private:
  Store& store_;
  const Plan& plan_;
  const IndexInfo& index_;
  std::vector<RID> rids_;
  std::size_t cursor_{0};
  bool open_{false};
};

class FilterExec final : public Executor {
 public:
  FilterExec(std::unique_ptr<Executor> child, const Plan& plan) : child_(std::move(child)), plan_(plan) {}

  void Open() override { child_->Open(); }
  void Close() override { child_->Close(); }

  bool Next(Tuple& row) override {
    Tuple candidate;
    while (child_->Next(candidate)) {
      if (plan_.predicate && EvalPred(*plan_.predicate, candidate, plan_.schema) == Truth::True) {
        row = std::move(candidate);
        return true;
      }
    }
    return false;
  }

 private:
  std::unique_ptr<Executor> child_;
  const Plan& plan_;
};

class ProjectExec final : public Executor {
 public:
  ProjectExec(std::unique_ptr<Executor> child, const Plan& plan) : child_(std::move(child)), plan_(plan) {}

  void Open() override { child_->Open(); }
  void Close() override { child_->Close(); }

  bool Next(Tuple& row) override {
    Tuple input;
    if (!child_->Next(input)) {
      return false;
    }
    const Schema& schema = plan_.left->schema;
    row.clear();
    row.reserve(plan_.project.size());
    for (const auto& expr : plan_.project) {
      row.push_back(EvalScalar(expr, input, schema));
    }
    return true;
  }

 private:
  std::unique_ptr<Executor> child_;
  const Plan& plan_;
};

class NestedLoopExec final : public Executor {
 public:
  NestedLoopExec(std::unique_ptr<Executor> left, std::unique_ptr<Executor> right)
      : left_(std::move(left)), right_(std::move(right)) {}

  void Open() override {
    left_->Open();
    have_left_ = left_->Next(left_row_);
    if (have_left_) {
      right_->Open();
      right_open_ = true;
    }
  }

  void Close() override {
    if (right_open_) {
      right_->Close();
      right_open_ = false;
    }
    left_->Close();
    have_left_ = false;
  }

  bool Next(Tuple& row) override {
    while (have_left_) {
      Tuple right_row;
      if (right_->Next(right_row)) {
        row = left_row_;
        row.insert(row.end(), right_row.begin(), right_row.end());
        return true;
      }
      right_->Close();
      right_open_ = false;
      have_left_ = left_->Next(left_row_);
      if (have_left_) {
        right_->Open();
        right_open_ = true;
      }
    }
    return false;
  }

 private:
  std::unique_ptr<Executor> left_;
  std::unique_ptr<Executor> right_;
  Tuple left_row_;
  bool have_left_{false};
  bool right_open_{false};
};

class HashJoinExec final : public Executor {
 public:
  HashJoinExec(std::unique_ptr<Executor> probe, std::unique_ptr<Executor> build, const Plan& plan)
      : probe_(std::move(probe)), build_(std::move(build)), plan_(plan) {}

  void Open() override {
    table_.clear();
    build_->Open();
    Tuple row;
    while (build_->Next(row)) {
      if (KeysNull(row, plan_.right_keys)) {
        continue;
      }
      table_[KeyOf(row, plan_.right_keys)].push_back(std::move(row));
    }
    build_->Close();
    probe_->Open();
    matches_ = nullptr;
    match_at_ = 0;
  }

  void Close() override { probe_->Close(); }

  bool Next(Tuple& row) override {
    while (true) {
      if (matches_ != nullptr && match_at_ < matches_->size()) {
        const Tuple& built = (*matches_)[match_at_++];
        row = probe_row_;
        row.insert(row.end(), built.begin(), built.end());
        return true;
      }
      matches_ = nullptr;
      if (!probe_->Next(probe_row_)) {
        return false;
      }
      if (KeysNull(probe_row_, plan_.left_keys)) {
        continue;
      }
      auto found = table_.find(KeyOf(probe_row_, plan_.left_keys));
      if (found == table_.end()) {
        continue;
      }
      matches_ = &found->second;
      match_at_ = 0;
    }
  }

 private:
  std::unique_ptr<Executor> probe_;
  std::unique_ptr<Executor> build_;
  const Plan& plan_;
  std::unordered_map<std::string, std::vector<Tuple>> table_;
  Tuple probe_row_;
  std::vector<Tuple>* matches_{nullptr};
  std::size_t match_at_{0};
};

struct AggAcc {
  int64_t count_rows{0};
  int64_t count_values{0};
  int64_t isum{0};
  double dsum{0};
  bool numeric_double{false};
  bool have_value{false};
  Value extreme;
};

class AggregateExec final : public Executor {
 public:
  AggregateExec(std::unique_ptr<Executor> child, const Plan& plan) : child_(std::move(child)), plan_(plan) {}

  void Open() override {
    groups_.clear();
    order_.clear();
    cursor_ = 0;
    child_->Open();
    Tuple row;
    bool any = false;
    while (child_->Next(row)) {
      any = true;
      std::vector<Value> keys;
      keys.reserve(plan_.group_exprs.size());
      for (const auto& expr : plan_.group_exprs) {
        keys.push_back(EvalScalar(expr, row, plan_.left->schema));
      }
      const std::string encoded = EncodeValues(keys);
      auto found = groups_.find(encoded);
      if (found == groups_.end()) {
        Group group;
        group.keys = std::move(keys);
        group.acc.resize(plan_.agg_exprs.size());
        found = groups_.emplace(encoded, std::move(group)).first;
        order_.push_back(encoded);
      }
      Accumulate(found->second, row);
    }
    child_->Close();
    if (!any && plan_.group_exprs.empty()) {
      Group group;
      group.acc.resize(plan_.agg_exprs.size());
      groups_.emplace("", std::move(group));
      order_.push_back("");
    }
    open_ = true;
  }

  void Close() override { open_ = false; }

  bool Next(Tuple& row) override {
    if (!open_) {
      throw Error("aggregate is not open");
    }
    if (cursor_ >= order_.size()) {
      return false;
    }
    const Group& group = groups_.at(order_[cursor_++]);
    row.clear();
    row.reserve(plan_.output_group.size());
    for (std::size_t i = 0; i < plan_.output_group.size(); ++i) {
      const int group_index = plan_.output_group[i];
      if (group_index >= 0) {
        row.push_back(group.keys[static_cast<std::size_t>(group_index)]);
      } else {
        row.push_back(Finish(group.acc[static_cast<std::size_t>(plan_.output_agg[i])],
                             plan_.agg_exprs[static_cast<std::size_t>(plan_.output_agg[i])]));
      }
    }
    return true;
  }

 private:
  struct Group {
    std::vector<Value> keys;
    std::vector<AggAcc> acc;
  };

  void Accumulate(Group& group, const Tuple& row) {
    for (std::size_t i = 0; i < plan_.agg_exprs.size(); ++i) {
      const Expr& expr = plan_.agg_exprs[i];
      AggAcc& acc = group.acc[i];
      ++acc.count_rows;
      if (expr.name == "*") {
        continue;
      }
      Value value = EvalScalar(*expr.left, row, plan_.left->schema);
      if (value.is_null) {
        continue;
      }
      ++acc.count_values;
      if (expr.op == "COUNT") {
        continue;
      }
      if (expr.op == "SUM" || expr.op == "AVG") {
        if (value.type == Type::Double || expr.op == "AVG") {
          const double add = value.type == Type::Double ? value.d : static_cast<double>(value.i);
          if (!acc.have_value && expr.op == "SUM" && value.type == Type::Integer) {
            acc.numeric_double = false;
          }
          if (value.type == Type::Double || acc.numeric_double || expr.op == "AVG") {
            if (!acc.numeric_double && acc.have_value) {
              acc.dsum = static_cast<double>(acc.isum);
            }
            acc.numeric_double = expr.op == "AVG" || value.type == Type::Double || acc.numeric_double;
            acc.dsum += add;
          } else {
            acc.isum += value.i;
          }
          acc.have_value = true;
          continue;
        }
        acc.isum += value.i;
        acc.have_value = true;
        continue;
      }
      if (expr.op == "MIN" || expr.op == "MAX") {
        if (!acc.have_value) {
          acc.extreme = value;
          acc.have_value = true;
        } else if (!value.is_null && !acc.extreme.is_null) {
          const int cmp = CompareValues(value, acc.extreme);
          if ((expr.op == "MIN" && cmp < 0) || (expr.op == "MAX" && cmp > 0)) {
            acc.extreme = value;
          }
        }
      }
    }
  }

  static Value Finish(const AggAcc& acc, const Expr& expr) {
    if (expr.op == "COUNT") {
      if (expr.name == "*") {
        return Value::Int(acc.count_rows);
      }
      return Value::Int(acc.count_values);
    }
    if (expr.op == "SUM") {
      if (!acc.have_value) {
        return Value::Null(acc.numeric_double ? Type::Double : Type::Integer);
      }
      if (acc.numeric_double) {
        return Value::Double(acc.dsum);
      }
      return Value::Int(acc.isum);
    }
    if (expr.op == "AVG") {
      if (acc.count_values == 0) {
        return Value::Null(Type::Double);
      }
      const double sum = acc.numeric_double ? acc.dsum : static_cast<double>(acc.isum);
      return Value::Double(sum / static_cast<double>(acc.count_values));
    }
    if (!acc.have_value) {
      return Value::Null(Type::Integer);
    }
    return acc.extreme;
  }

  std::unique_ptr<Executor> child_;
  const Plan& plan_;
  std::unordered_map<std::string, Group> groups_;
  std::vector<std::string> order_;
  std::size_t cursor_{0};
  bool open_{false};
};

class SortExec final : public Executor {
 public:
  SortExec(std::unique_ptr<Executor> child, const Plan& plan) : child_(std::move(child)), plan_(plan) {}

  void Open() override {
    rows_.clear();
    cursor_ = 0;
    child_->Open();
    Tuple row;
    while (child_->Next(row)) {
      rows_.push_back(std::move(row));
    }
    child_->Close();
    std::sort(rows_.begin(), rows_.end(), [&](const Tuple& a, const Tuple& b) {
      for (std::size_t i = 0; i < plan_.sort_cols.size(); ++i) {
        const int col = plan_.sort_cols[i];
        const int cmp = CompareNullsLow(a[static_cast<std::size_t>(col)], b[static_cast<std::size_t>(col)]);
        if (cmp == 0) {
          continue;
        }
        const bool asc = plan_.sort_asc[i];
        return asc ? cmp < 0 : cmp > 0;
      }
      return false;
    });
    open_ = true;
  }

  void Close() override { open_ = false; }

  bool Next(Tuple& row) override {
    if (cursor_ >= rows_.size()) {
      return false;
    }
    row = rows_[cursor_++];
    return true;
  }

 private:
  std::unique_ptr<Executor> child_;
  const Plan& plan_;
  std::vector<Tuple> rows_;
  std::size_t cursor_{0};
  bool open_{false};
};

class LimitExec final : public Executor {
 public:
  LimitExec(std::unique_ptr<Executor> child, const Plan& plan) : child_(std::move(child)), plan_(plan) {}

  void Open() override {
    child_->Open();
    seen_ = 0;
  }
  void Close() override { child_->Close(); }

  bool Next(Tuple& row) override {
    if (seen_ >= plan_.limit) {
      return false;
    }
    if (!child_->Next(row)) {
      return false;
    }
    ++seen_;
    return true;
  }

 private:
  std::unique_ptr<Executor> child_;
  const Plan& plan_;
  int64_t seen_{0};
};

const TableInfo& RequireTable(const Catalog& catalog, uint32_t id) {
  for (const auto& table : catalog.tables) {
    if (table.id == id) {
      return table;
    }
  }
  throw Error("plan references a missing table");
}

std::unique_ptr<Executor> Build(Store& store, const Catalog& catalog, const Plan& plan) {
  switch (plan.kind) {
    case Plan::Kind::SeqScan:
      return std::make_unique<SeqScanExec>(store, plan, RequireTable(catalog, plan.table_id));
    case Plan::Kind::IndexScan: {
      if (plan.index_slot >= catalog.indexes.size()) {
        throw Error("plan references a missing index");
      }
      return std::make_unique<IndexScanExec>(store, plan, catalog.indexes[plan.index_slot]);
    }
    case Plan::Kind::Filter:
      return std::make_unique<FilterExec>(Build(store, catalog, *plan.left), plan);
    case Plan::Kind::Project:
      return std::make_unique<ProjectExec>(Build(store, catalog, *plan.left), plan);
    case Plan::Kind::NestedLoop:
      return std::make_unique<NestedLoopExec>(Build(store, catalog, *plan.left),
                                              Build(store, catalog, *plan.right));
    case Plan::Kind::HashJoin:
      return std::make_unique<HashJoinExec>(Build(store, catalog, *plan.left),
                                            Build(store, catalog, *plan.right), plan);
    case Plan::Kind::IndexNL:
      throw Error("index nested-loop join is not produced by the planner");
    case Plan::Kind::Aggregate:
      return std::make_unique<AggregateExec>(Build(store, catalog, *plan.left), plan);
    case Plan::Kind::Sort:
      return std::make_unique<SortExec>(Build(store, catalog, *plan.left), plan);
    case Plan::Kind::Limit:
      return std::make_unique<LimitExec>(Build(store, catalog, *plan.left), plan);
  }
  throw Error("unknown plan node");
}

}  // namespace

std::unique_ptr<Executor> MakeExecutor(Store& store, const Catalog& catalog, const Plan& plan) {
  return Build(store, catalog, plan);
}

}  // namespace keel
