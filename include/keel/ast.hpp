#pragma once

#include "keel/types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace keel {

struct Expr {
  enum class Kind { Literal, Column, Binary, Unary, Aggregate, Star };
  Kind kind{Kind::Literal};
  std::string op;
  std::shared_ptr<Expr> left;
  std::shared_ptr<Expr> right;
  Value literal;
  std::string table;
  std::string name;
  std::string alias;
  int column_index{-1};

  static Expr Lit(Value value);
  static Expr Col(std::string table, std::string name);
  static Expr Bin(std::string op, Expr lhs, Expr rhs);
  static Expr Una(std::string op, Expr inner);
  static Expr Agg(std::string fn, Expr arg, bool star);
  static Expr Star(std::string table);
};

struct ColumnDef {
  std::string name;
  Type type{Type::Integer};
  uint32_t varchar_len{0};
  bool primary_key{false};
  bool not_null{false};
};

struct CreateTableData {
  std::string name;
  std::vector<ColumnDef> columns;
};

struct CreateIndexData {
  std::string name;
  std::string table;
  std::vector<std::string> columns;
  bool unique{false};
};

struct InsertData {
  std::string table;
  std::vector<std::string> columns;
  std::vector<std::vector<Expr>> rows;
};

struct Assign {
  std::string column;
  Expr value;
};

struct UpdateData {
  std::string table;
  std::vector<Assign> sets;
  std::shared_ptr<Expr> where;
};

struct DeleteData {
  std::string table;
  std::shared_ptr<Expr> where;
};

struct TableRef {
  std::string name;
  std::string alias;
};

struct JoinClause {
  TableRef table;
  std::shared_ptr<Expr> on;
};

struct OrderItem {
  Expr expr;
  bool asc{true};
};

struct SelectItem {
  Expr expr;
  bool star{false};
  std::string star_table;
};

struct SelectData {
  std::vector<SelectItem> items;
  TableRef from;
  std::vector<JoinClause> joins;
  std::shared_ptr<Expr> where;
  std::vector<Expr> group_by;
  std::vector<OrderItem> order_by;
  std::optional<int64_t> limit;
};

struct Statement {
  enum class Kind {
    CreateTable,
    CreateIndex,
    Insert,
    Update,
    Delete,
    Select,
    Begin,
    Commit,
    Rollback
  };
  Kind kind{Kind::Select};
  bool explain{false};
  bool analyze{false};
  CreateTableData create_table;
  CreateIndexData create_index;
  InsertData insert;
  UpdateData update;
  DeleteData del;
  SelectData select;
};

std::vector<Statement> ParseSql(const std::string& sql);
std::string ExprToString(const Expr& expr);

}  // namespace keel
