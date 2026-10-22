#pragma once

#include "keel/ast.hpp"

#include <string>

namespace keel {

enum class Truth { False, True, Unknown };

int ResolveColumn(const Schema& schema, const std::string& table, const std::string& name);
Value EvalScalar(const Expr& expr, const Tuple& row, const Schema& schema);
Truth EvalPred(const Expr& expr, const Tuple& row, const Schema& schema);
int CompareValues(const Value& left, const Value& right);

}  // namespace keel
