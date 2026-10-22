#pragma once

#include "keel/ast.hpp"
#include "keel/catalog.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace keel {

struct Plan {
  enum class Kind {
    SeqScan,
    IndexScan,
    Filter,
    Project,
    NestedLoop,
    HashJoin,
    IndexNL,
    Sort,
    Aggregate,
    Limit
  };

  Kind kind{Kind::SeqScan};
  std::string label;
  double cost{0};
  double rows{0};
  Schema schema;
  std::vector<std::string> names;
  std::unique_ptr<Plan> left;
  std::unique_ptr<Plan> right;

  uint32_t table_id{0};
  std::string table;
  std::string alias;
  std::size_t index_slot{0};
  std::string index_name;
  std::optional<std::string> low_key;
  std::optional<std::string> high_key;
  bool low_incl{true};
  bool high_incl{true};

  std::shared_ptr<Expr> predicate;
  std::vector<Expr> project;
  std::vector<int> left_keys;
  std::vector<int> right_keys;
  int outer_col{-1};
  int inner_col{-1};

  std::vector<Expr> group_exprs;
  std::vector<Expr> agg_exprs;
  std::vector<int> output_group;
  std::vector<int> output_agg;

  std::vector<int> sort_cols;
  std::vector<bool> sort_asc;
  int64_t limit{0};
};

std::unique_ptr<Plan> PlanQuery(const Catalog& catalog, const SelectData& query);
std::string Explain(const Plan& plan);

}  // namespace keel
