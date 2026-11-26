// expression.h - binding and evaluation of expressions.
#pragma once

#include <vector>

#include "ast.h"

namespace minidb {

// Returns a copy of `e` where every column reference is resolved to a position
// in `schema` (col_idx). Throws if a column is unknown or if `e` contains an aggregate.
ExprPtr BindExpr(const ExprPtr& e, const Schema& schema);

// Evaluates a bound expression against one row. Implements SQL three-valued logic.
Value Evaluate(const Expr& e, const Row& row);

inline bool IsTrue(const Value& v) { return v.Type() == TypeId::BOOL && v.AsBool(); }

bool ContainsAggregate(const Expr& e);
std::vector<ExprPtr> SplitConjuncts(const ExprPtr& e);  // a AND b AND c -> [a, b, c]
ExprPtr MakeConjunction(const std::vector<ExprPtr>& parts);
bool LikeMatch(const std::string& s, const std::string& pattern);
const char* BinOpSymbol(BinOp op);

}  // namespace minidb
