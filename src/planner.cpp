#include "keel/planner.hpp"

#include "keel/error.hpp"
#include "keel/expr.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace keel {
namespace {

constexpr double kSeqPageCost = 1.0;
constexpr double kIndexPageCost = 0.5;
constexpr double kHeapFetchCost = 0.15;
constexpr double kCpu = 0.01;
constexpr double kFanout = 80.0;

std::unique_ptr<Plan> ClonePlan(const Plan& src) {
  auto dst = std::make_unique<Plan>();
  dst->kind = src.kind;
  dst->label = src.label;
  dst->cost = src.cost;
  dst->rows = src.rows;
  dst->schema = src.schema;
  dst->names = src.names;
  dst->table_id = src.table_id;
  dst->table = src.table;
  dst->alias = src.alias;
  dst->index_slot = src.index_slot;
  dst->index_name = src.index_name;
  dst->low_key = src.low_key;
  dst->high_key = src.high_key;
  dst->low_incl = src.low_incl;
  dst->high_incl = src.high_incl;
  dst->predicate = src.predicate;
  dst->project = src.project;
  dst->left_keys = src.left_keys;
  dst->right_keys = src.right_keys;
  dst->outer_col = src.outer_col;
  dst->inner_col = src.inner_col;
  dst->group_exprs = src.group_exprs;
  dst->agg_exprs = src.agg_exprs;
  dst->output_group = src.output_group;
  dst->output_agg = src.output_agg;
  dst->sort_cols = src.sort_cols;
  dst->sort_asc = src.sort_asc;
  dst->limit = src.limit;
  if (src.left) {
    dst->left = ClonePlan(*src.left);
  }
  if (src.right) {
    dst->right = ClonePlan(*src.right);
  }
  return dst;
}

void Bind(Expr& expr, const Schema& schema) {
  if (expr.kind == Expr::Kind::Column) {
    expr.column_index = ResolveColumn(schema, expr.table, expr.name);
    if (expr.table.empty()) {
      expr.table = schema[expr.column_index].table;
    }
  }
  if (expr.left) {
    Bind(*expr.left, schema);
  }
  if (expr.right) {
    Bind(*expr.right, schema);
  }
}

Expr DeepCopy(const Expr& expr) {
  Expr copy = expr;
  copy.left.reset();
  copy.right.reset();
  if (expr.left) {
    copy.left = std::make_shared<Expr>(DeepCopy(*expr.left));
  }
  if (expr.right) {
    copy.right = std::make_shared<Expr>(DeepCopy(*expr.right));
  }
  return copy;
}

std::shared_ptr<Expr> BindCopy(const Expr& expr, const Schema& schema) {
  auto copy = std::make_shared<Expr>(DeepCopy(expr));
  Bind(*copy, schema);
  return copy;
}

void CollectTables(const Expr& expr, const Schema& schema, std::vector<std::string>& out) {
  if (expr.kind == Expr::Kind::Column) {
    const int idx = ResolveColumn(schema, expr.table, expr.name);
    const std::string& table = schema[idx].table;
    if (std::find(out.begin(), out.end(), table) == out.end()) {
      out.push_back(table);
    }
  }
  if (expr.left) {
    CollectTables(*expr.left, schema, out);
  }
  if (expr.right) {
    CollectTables(*expr.right, schema, out);
  }
}

void SplitAnd(const Expr& expr, std::vector<std::shared_ptr<Expr>>& out) {
  if (expr.kind == Expr::Kind::Binary && expr.op == "AND") {
    SplitAnd(*expr.left, out);
    SplitAnd(*expr.right, out);
    return;
  }
  out.push_back(std::make_shared<Expr>(expr));
}

bool ContainsAgg(const Expr& expr) {
  if (expr.kind == Expr::Kind::Aggregate) {
    return true;
  }
  return (expr.left && ContainsAgg(*expr.left)) || (expr.right && ContainsAgg(*expr.right));
}

Schema WithAlias(const Schema& schema, const std::string& alias) {
  Schema out = schema;
  for (auto& column : out) {
    column.table = alias;
  }
  return out;
}

std::vector<std::string> NamesOf(const Schema& schema) {
  std::vector<std::string> names;
  for (const auto& column : schema) {
    names.push_back(column.name);
  }
  return names;
}

double EqSelectivity(bool unique, double rows) {
  if (rows <= 1.0) {
    return 1.0;
  }
  if (unique) {
    return 1.0 / rows;
  }
  return std::min(1.0, 1.0 / std::sqrt(rows));
}

double PagesFor(const Schema& schema, double rows) {
  const double width = std::max(1.0, static_cast<double>(EstimateRowBytes(schema)));
  const double per = std::max(1.0, (static_cast<double>(kPageSize) - 64.0) / width);
  return std::max(1.0, rows / per);
}

struct Atom {
  int column{-1};
  std::string op;
  Value literal;
};

bool AsAtom(const Expr& expr, Atom& atom) {
  if (expr.kind != Expr::Kind::Binary) {
    return false;
  }
  if (expr.op != "=" && expr.op != "!=" && expr.op != "<" && expr.op != ">" && expr.op != "<=" &&
      expr.op != ">=") {
    return false;
  }
  const Expr* column = nullptr;
  const Expr* literal = nullptr;
  std::string op = expr.op;
  if (expr.left->kind == Expr::Kind::Column && expr.right->kind == Expr::Kind::Literal) {
    column = expr.left.get();
    literal = expr.right.get();
  } else if (expr.right->kind == Expr::Kind::Column && expr.left->kind == Expr::Kind::Literal) {
    column = expr.right.get();
    literal = expr.left.get();
    if (op == "<") op = ">";
    else if (op == ">") op = "<";
    else if (op == "<=") op = ">=";
    else if (op == ">=") op = "<=";
  } else {
    return false;
  }
  if (literal->literal.is_null || op == "!=") {
    return false;
  }
  atom.column = column->column_index;
  atom.op = op;
  atom.literal = literal->literal;
  return atom.column >= 0;
}

std::string EncodeBound(const std::vector<Value>& values) { return EncodeValues(values); }

struct Access {
  std::unique_ptr<Plan> plan;
  double final_rows{0};
};

Access PickAccess(const Catalog& catalog, const TableInfo& table, const std::string& alias,
                  const std::vector<std::shared_ptr<Expr>>& preds) {
  Schema schema = WithAlias(table.schema, alias);
  std::vector<std::shared_ptr<Expr>> bound;
  std::vector<Atom> atoms;
  double total_sel = 1.0;
  for (const auto& pred : preds) {
    auto copy = BindCopy(*pred, schema);
    Atom atom;
    if (AsAtom(*copy, atom)) {
      atoms.push_back(atom);
      if (atom.op == "=") {
        bool unique = false;
        for (const auto& index : catalog.indexes) {
          if (index.table_id == table.id && index.unique && index.columns.size() == 1 &&
              index.columns[0] == atom.column) {
            unique = true;
          }
        }
        if (table.schema[atom.column].primary_key) {
          unique = true;
        }
        total_sel *= EqSelectivity(unique, static_cast<double>(table.rows));
      } else {
        total_sel *= 0.3;
      }
    } else {
      total_sel *= 0.5;
    }
    bound.push_back(std::move(copy));
  }
  const double rows = static_cast<double>(table.rows);
  const double final_rows = rows == 0 ? 0 : std::max(rows * total_sel, total_sel < 1 ? 1.0 : rows);
  const double seq_cost = PagesFor(schema, std::max(rows, 1.0)) * kSeqPageCost + rows * kCpu;

  std::unique_ptr<Plan> best = std::make_unique<Plan>();
  best->kind = Plan::Kind::SeqScan;
  best->table_id = table.id;
  best->table = table.name;
  best->alias = alias;
  best->schema = schema;
  best->names = NamesOf(schema);
  best->rows = rows;
  best->cost = seq_cost;
  best->label = "single";

  double best_cost = seq_cost + final_rows * kCpu;
  for (std::size_t i = 0; i < catalog.indexes.size(); ++i) {
    const IndexInfo& index = catalog.indexes[i];
    if (index.table_id != table.id || index.columns.empty()) {
      continue;
    }
    std::vector<Value> prefix;
    std::optional<Value> low;
    std::optional<Value> high;
    bool low_incl = true;
    bool high_incl = true;
    bool any = false;
    double index_sel = 1.0;
    bool stopped = false;
    for (uint16_t column : index.columns) {
      if (stopped) {
        break;
      }
      const Atom* eq = nullptr;
      for (const auto& atom : atoms) {
        if (atom.column == column && atom.op == "=") {
          eq = &atom;
          break;
        }
      }
      if (eq != nullptr) {
        prefix.push_back(Coerce(eq->literal, table.schema[column]));
        bool unique = index.unique && index.columns.size() == prefix.size();
        index_sel *= EqSelectivity(unique || table.schema[column].primary_key, std::max(rows, 1.0));
        any = true;
        continue;
      }
      for (const auto& atom : atoms) {
        if (atom.column != column) {
          continue;
        }
        const Value value = Coerce(atom.literal, table.schema[column]);
        if (atom.op == ">" || atom.op == ">=") {
          low = value;
          low_incl = atom.op == ">=";
          any = true;
        } else if (atom.op == "<" || atom.op == "<=") {
          high = value;
          high_incl = atom.op == "<=";
          any = true;
        }
      }
      if (low || high) {
        index_sel *= (low && high) ? 0.1 : 0.3;
      }
      stopped = true;
    }
    if (!any) {
      continue;
    }
    const double index_out = rows == 0 ? 0 : std::max(rows * index_sel, 1.0);
    const double height = std::max(1.0, std::log(std::max(rows, 1.0)) / std::log(kFanout));
    const double leaves = std::max(1.0, index_out / kFanout);
    const double index_cost = height * kIndexPageCost + leaves * kIndexPageCost +
                              index_out * kHeapFetchCost + final_rows * kCpu;
    if (index_cost >= best_cost) {
      continue;
    }
    auto scan = std::make_unique<Plan>();
    scan->kind = Plan::Kind::IndexScan;
    scan->table_id = table.id;
    scan->table = table.name;
    scan->alias = alias;
    scan->schema = schema;
    scan->names = NamesOf(schema);
    scan->rows = index_out;
    scan->cost = index_cost;
    scan->index_slot = i;
    scan->index_name = index.name;
    scan->label = "single";
    if (low) {
      auto values = prefix;
      values.push_back(*low);
      scan->low_key = EncodeBound(values);
      scan->low_incl = low_incl;
    } else if (!prefix.empty()) {
      scan->low_key = EncodeBound(prefix);
      scan->low_incl = true;
    }
    if (high) {
      auto values = prefix;
      values.push_back(*high);
      scan->high_key = EncodeBound(values);
      scan->high_incl = high_incl;
    } else if (!prefix.empty() && !low) {
      scan->high_key = EncodeBound(prefix);
      scan->high_incl = true;
    } else if (!prefix.empty() && low) {
      scan->high_key = EncodeBound(prefix);
      scan->high_incl = true;
    }
    best = std::move(scan);
    best_cost = index_cost;
  }

  Access access;
  access.final_rows = preds.empty() ? rows : final_rows;
  if (!preds.empty()) {
    auto filter = std::make_unique<Plan>();
    filter->kind = Plan::Kind::Filter;
    filter->schema = best->schema;
    filter->names = best->names;
    filter->table_id = table.id;
    filter->table = table.name;
    filter->alias = alias;
    filter->rows = access.final_rows;
    filter->cost = best_cost;
    filter->label = "single";
    Expr combined = *bound.front();
    for (std::size_t i = 1; i < bound.size(); ++i) {
      combined = Expr::Bin("AND", std::move(combined), *bound[i]);
    }
    filter->predicate = std::make_shared<Expr>(std::move(combined));
    filter->left = std::move(best);
    access.plan = std::move(filter);
  } else {
    best->rows = rows;
    access.plan = std::move(best);
  }
  return access;
}

std::vector<std::string> TablesOf(const Expr& expr, const Schema& schema) {
  std::vector<std::string> tables;
  CollectTables(expr, schema, tables);
  return tables;
}

std::unique_ptr<Plan> MakeJoin(const Plan& left, const Plan& right,
                               const std::vector<std::shared_ptr<Expr>>& conjuncts) {
  Schema schema = left.schema;
  schema.insert(schema.end(), right.schema.begin(), right.schema.end());
  std::vector<std::string> names = left.names;
  names.insert(names.end(), right.names.begin(), right.names.end());
  std::vector<std::shared_ptr<Expr>> crossing;
  for (const auto& conjunct : conjuncts) {
    auto tables = TablesOf(*conjunct, schema);
    bool hit_left = false;
    bool hit_right = false;
    for (const auto& table : tables) {
      for (const auto& column : left.schema) {
        if (column.table == table) hit_left = true;
      }
      for (const auto& column : right.schema) {
        if (column.table == table) hit_right = true;
      }
    }
    if (hit_left && hit_right) {
      crossing.push_back(BindCopy(*conjunct, schema));
    }
  }

  std::vector<int> left_keys;
  std::vector<int> right_keys;
  for (const auto& expr : crossing) {
    if (expr->kind != Expr::Kind::Binary || expr->op != "=" ||
        expr->left->kind != Expr::Kind::Column || expr->right->kind != Expr::Kind::Column) {
      continue;
    }
    const bool left_on_left = expr->left->column_index < static_cast<int>(left.schema.size());
    const bool right_on_right = expr->right->column_index >= static_cast<int>(left.schema.size());
    const bool swapped = expr->left->column_index >= static_cast<int>(left.schema.size()) &&
                         expr->right->column_index < static_cast<int>(left.schema.size());
    if (left_on_left && right_on_right) {
      left_keys.push_back(expr->left->column_index);
      right_keys.push_back(expr->right->column_index - static_cast<int>(left.schema.size()));
    } else if (swapped) {
      left_keys.push_back(expr->right->column_index);
      right_keys.push_back(expr->left->column_index - static_cast<int>(left.schema.size()));
    }
  }

  // Equality keys drive the hash join. Every conjunct is re-applied by the
  // filter above a multi-table plan, against the final output schema, so a
  // residual predicate is not attached here (a swapped build side would make
  // pre-swap column indexes point at the wrong fields).
  const double estimated = left_keys.empty() ? left.rows * right.rows : std::max(left.rows, right.rows);
  std::unique_ptr<Plan> best;
  if (!left_keys.empty()) {
    auto hash = std::make_unique<Plan>();
    hash->kind = Plan::Kind::HashJoin;
    const bool build_left = left.rows < right.rows;
    if (build_left) {
      hash->left = ClonePlan(right);
      hash->right = ClonePlan(left);
      hash->schema = right.schema;
      hash->schema.insert(hash->schema.end(), left.schema.begin(), left.schema.end());
      hash->names = right.names;
      hash->names.insert(hash->names.end(), left.names.begin(), left.names.end());
      for (std::size_t k = 0; k < left_keys.size(); ++k) {
        hash->left_keys.push_back(right_keys[k]);
        hash->right_keys.push_back(left_keys[k]);
      }
      hash->label = left.alias.empty() ? left.table : left.alias;
    } else {
      hash->left = ClonePlan(left);
      hash->right = ClonePlan(right);
      hash->schema = schema;
      hash->names = names;
      hash->left_keys = left_keys;
      hash->right_keys = right_keys;
      hash->label = right.alias.empty() ? right.table : right.alias;
    }
    hash->cost = left.cost + right.cost + (left.rows + right.rows) * kCpu * 2.0;
    hash->rows = estimated;
    best = std::move(hash);
  }

  if (!best) {
    auto nest = std::make_unique<Plan>();
    nest->kind = Plan::Kind::NestedLoop;
    nest->left = ClonePlan(left);
    nest->right = ClonePlan(right);
    nest->schema = schema;
    nest->names = names;
    nest->rows = left.rows * right.rows;
    nest->cost = left.cost + std::max(left.rows, 1.0) * right.cost +
                 std::max(left.rows, 1.0) * std::max(right.rows, 1.0) * kCpu;
    nest->label = "nested";
    best = std::move(nest);
  }
  return best;
}

void ExplainRec(const Plan& plan, std::ostringstream& out, int indent) {
  out << std::string(indent * 2, ' ');
  switch (plan.kind) {
    case Plan::Kind::SeqScan:
      out << "SeqScan table=" << plan.table << " alias=" << plan.alias;
      break;
    case Plan::Kind::IndexScan:
      out << "IndexScan table=" << plan.table << " alias=" << plan.alias << " index=" << plan.index_name;
      break;
    case Plan::Kind::Filter:
      out << "Filter";
      if (plan.predicate) {
        out << " pred=" << ExprToString(*plan.predicate);
      }
      break;
    case Plan::Kind::Project:
      out << "Project";
      break;
    case Plan::Kind::NestedLoop:
      out << "NestedLoopJoin";
      break;
    case Plan::Kind::HashJoin:
      out << "HashJoin build=" << plan.label;
      break;
    case Plan::Kind::IndexNL:
      out << "IndexNestedLoopJoin index=" << plan.index_name;
      break;
    case Plan::Kind::Sort:
      out << "Sort";
      break;
    case Plan::Kind::Aggregate:
      out << "Aggregate";
      break;
    case Plan::Kind::Limit:
      out << "Limit " << plan.limit;
      break;
  }
  out << " est_rows=" << plan.rows << " cost=" << plan.cost << "\n";
  if (plan.left) {
    ExplainRec(*plan.left, out, indent + 1);
  }
  if (plan.right) {
    ExplainRec(*plan.right, out, indent + 1);
  }
}

Type ExprType(const Expr& expr, const Schema& schema) {
  switch (expr.kind) {
    case Expr::Kind::Literal:
      return expr.literal.type;
    case Expr::Kind::Column:
      return schema[expr.column_index].type;
    case Expr::Kind::Aggregate:
      if (expr.op == "COUNT") return Type::Integer;
      if (expr.op == "AVG") return Type::Double;
      if (expr.op == "SUM") {
        if (expr.left && expr.left->kind == Expr::Kind::Column) {
          return schema[expr.left->column_index].type == Type::Double ? Type::Double : Type::Integer;
        }
        return Type::Double;
      }
      if (expr.left && expr.left->kind == Expr::Kind::Column) {
        return schema[expr.left->column_index].type;
      }
      return Type::Integer;
    case Expr::Kind::Unary:
      if (expr.op == "NEG" && expr.left) return ExprType(*expr.left, schema);
      return Type::Boolean;
    case Expr::Kind::Binary:
      if (expr.op == "+" || expr.op == "-" || expr.op == "*") {
        const Type lt = ExprType(*expr.left, schema);
        const Type rt = ExprType(*expr.right, schema);
        if (lt == Type::Double || rt == Type::Double) return Type::Double;
        return Type::Integer;
      }
      if (expr.op == "/") return Type::Double;
      return Type::Boolean;
    case Expr::Kind::Star:
      return Type::Integer;
  }
  return Type::Integer;
}

}  // namespace

std::unique_ptr<Plan> PlanQuery(const Catalog& catalog, const SelectData& query) {
  if (query.items.empty()) {
    throw Error("SELECT list is empty");
  }
  std::vector<TableRef> refs;
  refs.push_back(query.from);
  std::vector<std::shared_ptr<Expr>> conjuncts;
  for (const auto& join : query.joins) {
    refs.push_back(join.table);
    if (join.on) {
      SplitAnd(*join.on, conjuncts);
    }
  }
  if (query.where) {
    SplitAnd(*query.where, conjuncts);
  }
  if (query.limit && *query.limit < 0) {
    throw Error("LIMIT must be non-negative");
  }
  if (refs.size() > 8) {
    throw Error("joins of more than 8 tables are not supported");
  }
  for (std::size_t i = 0; i < refs.size(); ++i) {
    for (std::size_t j = i + 1; j < refs.size(); ++j) {
      if (refs[i].alias == refs[j].alias) {
        throw Error("duplicate table alias " + refs[i].alias);
      }
    }
  }

  Schema universe;
  std::vector<const TableInfo*> tables;
  for (const auto& ref : refs) {
    const TableInfo* table = catalog.FindTable(ref.name);
    if (table == nullptr) {
      throw Error("no such table " + ref.name);
    }
    tables.push_back(table);
    Schema part = WithAlias(table->schema, ref.alias);
    universe.insert(universe.end(), part.begin(), part.end());
  }

  std::vector<std::vector<std::shared_ptr<Expr>>> local(refs.size());
  std::vector<std::shared_ptr<Expr>> joins;
  for (const auto& conjunct : conjuncts) {
    const auto mentioned = TablesOf(*conjunct, universe);
    if (mentioned.size() <= 1) {
      std::string alias = mentioned.empty() ? refs[0].alias : mentioned[0];
      if (mentioned.empty() && refs.size() != 1) {
        // Unqualified columns were resolved against the whole FROM clause.
        alias = mentioned.empty() ? std::string() : mentioned[0];
      }
      int slot = -1;
      if (!mentioned.empty()) {
        for (std::size_t i = 0; i < refs.size(); ++i) {
          if (refs[i].alias == mentioned[0]) slot = static_cast<int>(i);
        }
      } else if (refs.size() == 1) {
        slot = 0;
      }
      if (slot < 0) {
        throw Error("column reference does not match a single table");
      }
      local[slot].push_back(conjunct);
    } else {
      joins.push_back(conjunct);
    }
  }

  const int n = static_cast<int>(refs.size());
  std::vector<std::unique_ptr<Plan>> best(1u << n);
  for (int i = 0; i < n; ++i) {
    Access access = PickAccess(catalog, *tables[i], refs[i].alias, local[i]);
    best[1u << i] = std::move(access.plan);
  }
  for (int mask = 1; mask < (1 << n); ++mask) {
    if (best[mask]) {
      continue;
    }
    for (int sub = (mask - 1) & mask; sub > 0; sub = (sub - 1) & mask) {
      const int other = mask ^ sub;
      if (!best[sub] || !best[other]) {
        continue;
      }
      bool connected = false;
      Schema both = best[sub]->schema;
      both.insert(both.end(), best[other]->schema.begin(), best[other]->schema.end());
      for (const auto& conjunct : joins) {
        auto tables_in = TablesOf(*conjunct, universe);
        bool hit_l = false;
        bool hit_r = false;
        for (const auto& name : tables_in) {
          for (const auto& column : best[sub]->schema) {
            if (column.table == name) hit_l = true;
          }
          for (const auto& column : best[other]->schema) {
            if (column.table == name) hit_r = true;
          }
        }
        if (hit_l && hit_r) {
          connected = true;
        }
      }
      if (!connected && mask != (1 << n) - 1) {
        continue;
      }
      auto joined = MakeJoin(*best[sub], *best[other], joins);
      if (!best[mask] || joined->cost < best[mask]->cost) {
        best[mask] = std::move(joined);
      }
    }
  }
  if (!best[(1 << n) - 1]) {
    throw Error("could not plan join");
  }
  std::unique_ptr<Plan> from = std::move(best[(1 << n) - 1]);
  if (!conjuncts.empty() && n > 1) {
    auto filter = std::make_unique<Plan>();
    filter->kind = Plan::Kind::Filter;
    filter->left = std::move(from);
    filter->schema = filter->left->schema;
    filter->names = filter->left->names;
    filter->rows = filter->left->rows;
    filter->cost = filter->left->cost + filter->rows * kCpu;
    Expr combined = *conjuncts.front();
    for (std::size_t i = 1; i < conjuncts.size(); ++i) {
      combined = Expr::Bin("AND", std::move(combined), *conjuncts[i]);
    }
    filter->predicate = BindCopy(combined, filter->schema);
    from = std::move(filter);
  }

  const bool grouped = !query.group_by.empty();
  bool aggregated = grouped;
  for (const auto& item : query.items) {
    if (!item.star && ContainsAgg(item.expr)) {
      aggregated = true;
    }
  }

  std::unique_ptr<Plan> projected;
  if (aggregated) {
    if (query.items.size() == 1 && query.items[0].star) {
      throw Error("SELECT * cannot be combined with GROUP BY or aggregates");
    }
    std::vector<Expr> groups;
    for (const auto& expr : query.group_by) {
      if (expr.kind != Expr::Kind::Column) {
        throw Error("GROUP BY supports columns only");
      }
      groups.push_back(*BindCopy(expr, from->schema));
    }
    auto agg = std::make_unique<Plan>();
    agg->kind = Plan::Kind::Aggregate;
    agg->group_exprs = groups;
    for (const auto& item : query.items) {
      if (item.star) {
        throw Error("SELECT * cannot be combined with GROUP BY or aggregates");
      }
      if (item.expr.kind == Expr::Kind::Aggregate) {
        Expr copy = *BindCopy(item.expr, from->schema);
        if (copy.left) {
          Bind(*copy.left, from->schema);
        }
        agg->output_group.push_back(-1);
        agg->output_agg.push_back(static_cast<int>(agg->agg_exprs.size()));
        agg->agg_exprs.push_back(std::move(copy));
        Column column;
        column.name = item.expr.alias.empty() ? ExprToString(item.expr) : item.expr.alias;
        column.type = ExprType(agg->agg_exprs.back(), from->schema);
        agg->schema.push_back(column);
        agg->names.push_back(column.name);
      } else if (item.expr.kind == Expr::Kind::Column) {
        Expr copy = *BindCopy(item.expr, from->schema);
        int group_index = -1;
        for (int g = 0; g < static_cast<int>(groups.size()); ++g) {
          if (groups[g].column_index == copy.column_index) {
            group_index = g;
          }
        }
        if (group_index < 0) {
          throw Error("column " + item.expr.name + " must appear in GROUP BY");
        }
        agg->output_group.push_back(group_index);
        agg->output_agg.push_back(-1);
        Column column = from->schema[copy.column_index];
        column.name = item.expr.alias.empty() ? column.name : item.expr.alias;
        if (!item.expr.alias.empty()) {
          column.table.clear();
        }
        agg->schema.push_back(column);
        agg->names.push_back(column.name);
      } else if (!ContainsAgg(item.expr)) {
        throw Error("non-aggregate expression must be a GROUP BY column");
      } else {
        throw Error("aggregates must be the top-level expression in the select list");
      }
    }
    agg->rows = groups.empty() ? 1.0 : std::max(1.0, from->rows / 4.0);
    agg->cost = from->cost + from->rows * kCpu;
    agg->left = std::move(from);
    projected = std::move(agg);
  } else {
    auto project = std::make_unique<Plan>();
    project->kind = Plan::Kind::Project;
    for (const auto& item : query.items) {
      if (item.star || item.expr.kind == Expr::Kind::Star) {
        const std::string& only = item.star ? item.star_table : item.expr.table;
        for (std::size_t i = 0; i < from->schema.size(); ++i) {
          if (!only.empty() && from->schema[i].table != only) {
            continue;
          }
          Expr col = Expr::Col(from->schema[i].table, from->schema[i].name);
          col.column_index = static_cast<int>(i);
          project->project.push_back(col);
          project->schema.push_back(from->schema[i]);
          project->names.push_back(from->schema[i].name);
        }
        continue;
      }
      if (ContainsAgg(item.expr)) {
        throw Error("aggregates must be the top-level expression in the select list");
      }
      Expr copy = *BindCopy(item.expr, from->schema);
      Column column;
      column.name = item.expr.alias.empty() ? (copy.kind == Expr::Kind::Column ? copy.name : ExprToString(copy))
                                           : item.expr.alias;
      column.type = ExprType(copy, from->schema);
      if (copy.kind == Expr::Kind::Column && item.expr.alias.empty()) {
        column.table = from->schema[copy.column_index].table;
      }
      project->project.push_back(std::move(copy));
      project->schema.push_back(column);
      project->names.push_back(column.name);
    }
    project->rows = from->rows;
    project->cost = from->cost + from->rows * kCpu;
    project->left = std::move(from);
    projected = std::move(project);
  }

  if (!query.order_by.empty()) {
    const Schema& input = projected->left ? projected->left->schema : projected->schema;
    struct SortKey {
      int index{-1};
      bool asc{true};
      bool from_input{false};
    };
    std::vector<SortKey> keys;
    for (const auto& item : query.order_by) {
      SortKey key;
      key.asc = item.asc;
      if (item.expr.kind == Expr::Kind::Literal && item.expr.literal.type == Type::Integer &&
          !item.expr.literal.is_null) {
        const int pos = static_cast<int>(item.expr.literal.i) - 1;
        if (pos < 0 || pos >= static_cast<int>(projected->schema.size())) {
          throw Error("ORDER BY position is out of range");
        }
        key.index = pos;
      } else if (item.expr.kind == Expr::Kind::Column) {
        int pos = -1;
        for (int i = 0; i < static_cast<int>(projected->names.size()); ++i) {
          if (projected->names[i] == item.expr.name &&
              (item.expr.table.empty() || projected->schema[i].table == item.expr.table)) {
            if (pos != -1) {
              throw Error("ambiguous ORDER BY " + item.expr.name);
            }
            pos = i;
          }
        }
        if (pos >= 0) {
          key.index = pos;
        } else if (projected->kind == Plan::Kind::Project) {
          key.index = ResolveColumn(input, item.expr.table, item.expr.name);
          key.from_input = true;
        } else {
          throw Error("ORDER BY column " + item.expr.name + " is not in the select list");
        }
      } else {
        throw Error("ORDER BY supports columns and select-list positions");
      }
      keys.push_back(key);
    }

    const int visible = static_cast<int>(projected->schema.size());
    bool needs_input = false;
    for (const auto& key : keys) {
      if (key.from_input) {
        needs_input = true;
      }
    }
    if (needs_input) {
      for (auto& key : keys) {
        if (!key.from_input) {
          continue;
        }
        int out_index = -1;
        for (int i = 0; i < static_cast<int>(projected->project.size()); ++i) {
          const Expr& expr = projected->project[static_cast<std::size_t>(i)];
          if (expr.kind == Expr::Kind::Column && expr.column_index == key.index) {
            out_index = i;
            break;
          }
        }
        if (out_index < 0) {
          Expr col = Expr::Col(input[static_cast<std::size_t>(key.index)].table,
                               input[static_cast<std::size_t>(key.index)].name);
          col.column_index = key.index;
          projected->project.push_back(std::move(col));
          projected->schema.push_back(input[static_cast<std::size_t>(key.index)]);
          projected->names.push_back(input[static_cast<std::size_t>(key.index)].name);
          out_index = static_cast<int>(projected->project.size()) - 1;
        }
        key.index = out_index;
        key.from_input = false;
      }
    }

    auto sort = std::make_unique<Plan>();
    sort->kind = Plan::Kind::Sort;
    sort->schema = projected->schema;
    sort->names = projected->names;
    sort->rows = projected->rows;
    sort->cost = projected->cost + projected->rows * std::log2(std::max(projected->rows, 2.0)) * kCpu;
    for (const auto& key : keys) {
      sort->sort_cols.push_back(key.index);
      sort->sort_asc.push_back(key.asc);
    }
    sort->left = std::move(projected);
    projected = std::move(sort);
    if (needs_input && visible < static_cast<int>(projected->schema.size())) {
      auto strip = std::make_unique<Plan>();
      strip->kind = Plan::Kind::Project;
      strip->rows = projected->rows;
      strip->cost = projected->cost;
      for (int i = 0; i < visible; ++i) {
        Expr col = Expr::Col(projected->schema[static_cast<std::size_t>(i)].table,
                             projected->names[static_cast<std::size_t>(i)]);
        col.column_index = i;
        strip->project.push_back(std::move(col));
        strip->schema.push_back(projected->schema[static_cast<std::size_t>(i)]);
        strip->names.push_back(projected->names[static_cast<std::size_t>(i)]);
      }
      strip->left = std::move(projected);
      projected = std::move(strip);
    }
  }

  if (query.limit) {
    auto limit = std::make_unique<Plan>();
    limit->kind = Plan::Kind::Limit;
    limit->limit = *query.limit;
    limit->schema = projected->schema;
    limit->names = projected->names;
    limit->rows = std::min(projected->rows, static_cast<double>(*query.limit));
    limit->cost = projected->cost;
    limit->left = std::move(projected);
    projected = std::move(limit);
  }
  return projected;
}

std::string Explain(const Plan& plan) {
  std::ostringstream out;
  ExplainRec(plan, out, 0);
  return out.str();
}

}  // namespace keel
