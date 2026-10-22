#include "keel/expr.hpp"

#include "keel/error.hpp"

#include <cmath>

namespace keel {
namespace {

bool Numeric(Type type) { return type == Type::Integer || type == Type::Double; }

}  // namespace

int ResolveColumn(const Schema& schema, const std::string& table, const std::string& name) {
  int found = -1;
  for (int i = 0; i < static_cast<int>(schema.size()); ++i) {
    if (!table.empty() && schema[i].table != table) {
      continue;
    }
    if (schema[i].name != name) {
      continue;
    }
    if (found != -1) {
      throw Error("ambiguous column " + name);
    }
    found = i;
  }
  if (found < 0) {
    throw Error("no such column " + (table.empty() ? name : table + "." + name));
  }
  return found;
}

int CompareValues(const Value& left, const Value& right) {
  if (left.is_null || right.is_null) {
    throw Error("cannot compare null with CompareValues");
  }
  if (Numeric(left.type) && Numeric(right.type)) {
    if (left.type == Type::Integer && right.type == Type::Integer) {
      if (left.i < right.i) return -1;
      if (left.i > right.i) return 1;
      return 0;
    }
    const double a = left.type == Type::Double ? left.d : static_cast<double>(left.i);
    const double b = right.type == Type::Double ? right.d : static_cast<double>(right.i);
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }
  if (left.type == Type::Varchar && right.type == Type::Varchar) {
    if (left.s < right.s) return -1;
    if (left.s > right.s) return 1;
    return 0;
  }
  if (left.type == Type::Boolean && right.type == Type::Boolean) {
    if (left.b == right.b) return 0;
    return left.b ? 1 : -1;
  }
  throw Error(std::string("cannot compare ") + TypeName(left.type) + " and " + TypeName(right.type));
}

Value EvalScalar(const Expr& expr, const Tuple& row, const Schema& schema) {
  switch (expr.kind) {
    case Expr::Kind::Literal:
      return expr.literal;
    case Expr::Kind::Column: {
      const int idx = expr.column_index >= 0 ? expr.column_index
                                             : ResolveColumn(schema, expr.table, expr.name);
      if (idx < 0 || idx >= static_cast<int>(row.size())) {
        throw Error("column index out of range");
      }
      return row[idx];
    }
    case Expr::Kind::Star:
      throw Error("* is not a scalar expression");
    case Expr::Kind::Aggregate:
      throw Error("aggregate " + expr.op + " is not allowed in this expression");
    case Expr::Kind::Unary: {
      if (expr.op == "NEG") {
        const Value inner = EvalScalar(*expr.left, row, schema);
        if (inner.is_null) {
          return inner;
        }
        if (inner.type == Type::Integer) {
          return Value::Int(-inner.i);
        }
        if (inner.type == Type::Double) {
          return Value::Double(-inner.d);
        }
        throw Error("unary minus needs a number");
      }
      if (expr.op == "IS NULL") {
        return Value::Bool(EvalScalar(*expr.left, row, schema).is_null);
      }
      if (expr.op == "IS NOT NULL") {
        return Value::Bool(!EvalScalar(*expr.left, row, schema).is_null);
      }
      if (expr.op == "NOT") {
        const Truth truth = EvalPred(*expr.left, row, schema);
        if (truth == Truth::Unknown) {
          return Value::Null(Type::Boolean);
        }
        return Value::Bool(truth == Truth::False);
      }
      throw Error("unknown unary operator " + expr.op);
    }
    case Expr::Kind::Binary: {
      if (expr.op == "AND" || expr.op == "OR") {
        const Truth truth = EvalPred(expr, row, schema);
        if (truth == Truth::Unknown) {
          return Value::Null(Type::Boolean);
        }
        return Value::Bool(truth == Truth::True);
      }
      const Value left = EvalScalar(*expr.left, row, schema);
      const Value right = EvalScalar(*expr.right, row, schema);
      if (expr.op == "=" || expr.op == "!=" || expr.op == "<" || expr.op == ">" || expr.op == "<=" ||
          expr.op == ">=") {
        if (left.is_null || right.is_null) {
          return Value::Null(Type::Boolean);
        }
        const int cmp = CompareValues(left, right);
        bool ok = false;
        if (expr.op == "=") ok = cmp == 0;
        else if (expr.op == "!=") ok = cmp != 0;
        else if (expr.op == "<") ok = cmp < 0;
        else if (expr.op == ">") ok = cmp > 0;
        else if (expr.op == "<=") ok = cmp <= 0;
        else ok = cmp >= 0;
        return Value::Bool(ok);
      }
      if (left.is_null || right.is_null) {
        return Value::Null(left.is_null ? left.type : right.type);
      }
      if (!Numeric(left.type) || !Numeric(right.type)) {
        throw Error("arithmetic needs numbers");
      }
      const bool use_double = left.type == Type::Double || right.type == Type::Double || expr.op == "/";
      if (!use_double) {
        if (expr.op == "+") return Value::Int(left.i + right.i);
        if (expr.op == "-") return Value::Int(left.i - right.i);
        if (expr.op == "*") return Value::Int(left.i * right.i);
      }
      const double a = left.type == Type::Double ? left.d : static_cast<double>(left.i);
      const double b = right.type == Type::Double ? right.d : static_cast<double>(right.i);
      if (expr.op == "/" && b == 0.0) {
        throw Error("division by zero");
      }
      double result = 0;
      if (expr.op == "+") result = a + b;
      else if (expr.op == "-") result = a - b;
      else if (expr.op == "*") result = a * b;
      else if (expr.op == "/") result = a / b;
      else throw Error("unknown operator " + expr.op);
      if (!use_double) {
        return Value::Int(static_cast<int64_t>(result));
      }
      return Value::Double(result);
    }
  }
  throw Error("bad expression");
}

Truth EvalPred(const Expr& expr, const Tuple& row, const Schema& schema) {
  if (expr.kind == Expr::Kind::Binary && expr.op == "AND") {
    const Truth left = EvalPred(*expr.left, row, schema);
    const Truth right = EvalPred(*expr.right, row, schema);
    if (left == Truth::False || right == Truth::False) return Truth::False;
    if (left == Truth::Unknown || right == Truth::Unknown) return Truth::Unknown;
    return Truth::True;
  }
  if (expr.kind == Expr::Kind::Binary && expr.op == "OR") {
    const Truth left = EvalPred(*expr.left, row, schema);
    const Truth right = EvalPred(*expr.right, row, schema);
    if (left == Truth::True || right == Truth::True) return Truth::True;
    if (left == Truth::Unknown || right == Truth::Unknown) return Truth::Unknown;
    return Truth::False;
  }
  if (expr.kind == Expr::Kind::Unary && expr.op == "NOT") {
    const Truth inner = EvalPred(*expr.left, row, schema);
    if (inner == Truth::Unknown) return Truth::Unknown;
    return inner == Truth::True ? Truth::False : Truth::True;
  }
  if (expr.kind == Expr::Kind::Unary && (expr.op == "IS NULL" || expr.op == "IS NOT NULL")) {
    const bool is_null = EvalScalar(*expr.left, row, schema).is_null;
    const bool yes = expr.op == "IS NULL" ? is_null : !is_null;
    return yes ? Truth::True : Truth::False;
  }
  const Value value = EvalScalar(expr, row, schema);
  if (value.is_null) {
    return Truth::Unknown;
  }
  if (value.type == Type::Boolean) {
    return value.b ? Truth::True : Truth::False;
  }
  throw Error("WHERE expression is not a predicate");
}

}  // namespace keel
