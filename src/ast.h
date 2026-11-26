// ast.h - abstract syntax tree produced by the SQL parser.
#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include "value.h"

namespace minidb {

enum class ExprKind { CONST, COLUMN, BINARY, UNARY, IS_NULL, AGG, FUNC, SUBQUERY, IN_SET, PARAM, SUBPLAN };
enum class SubqueryKind { SCALAR, EXISTS, IN };
enum class BinOp { EQ, NE, LT, LE, GT, GE, AND, OR, ADD, SUB, MUL, DIV, MOD, LIKE, CONCAT };
enum class UnaryOp { NOT, NEG };
enum class AggFunc { COUNT, SUM, AVG, MIN, MAX };

struct Expr;
struct SelectStmt;
class Executor;

// Every pass over an expression (binding, evaluation, printing, even the
// destructor) is recursive, so a tree that is too tall would overflow the
// stack and kill the process. The factories below refuse to build one.
constexpr int kMaxExprHeight = 1000;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
    ExprKind kind = ExprKind::CONST;
    Value value;                 // CONST
    std::string table, column;   // COLUMN (table may be empty)
    int col_idx = -1;            // COLUMN: position in the input row, set by binding
    bool prebound = false;       // COLUMN created by the planner, already bound
    BinOp op = BinOp::EQ;        // BINARY
    UnaryOp uop = UnaryOp::NOT;  // UNARY
    bool negated = false;        // IS_NULL: true means IS NOT NULL
    AggFunc agg = AggFunc::COUNT;
    bool agg_star = false;       // COUNT(*)
    bool agg_distinct = false;   // COUNT(DISTINCT x): each distinct value counted once
    // FUNC: scalar function call `func(args...)`. CASE is func "case" with args
    // [cond1, val1, cond2, val2, ...] plus a trailing ELSE value if args.size() is odd.
    std::string func;
    std::vector<ExprPtr> args;
    // SUBQUERY (parser output): (SELECT ...), EXISTS (SELECT ...) or
    // `left` [NOT] IN (SELECT ...); `negated` for NOT EXISTS / NOT IN. The planner
    // runs the (uncorrelated) subquery once and replaces the node with a CONST,
    // or for IN with an IN_SET node: `left` IN in_set, where in_set holds the
    // Value::AppendKey encodings of the non-NULL results.
    SubqueryKind sub_kind = SubqueryKind::SCALAR;
    std::shared_ptr<const SelectStmt> subquery;
    std::shared_ptr<const std::unordered_set<std::string>> in_set;
    bool in_set_has_null = false;
    // Correlated subqueries. PARAM: a column of an enclosing query, read from
    // (*params)[param_slot] (`column` keeps the name for printing). SUBPLAN: a
    // planned subquery (`subplan`, same kinds as SUBQUERY) that is re-run for every
    // outer row after copying the values of `args` (outer column expressions, bound
    // against the outer row) into the `params` its PARAM nodes read.
    std::shared_ptr<std::vector<Value>> params;
    int param_slot = -1;
    std::shared_ptr<Executor> subplan;
    ExprPtr left, right;         // children (UNARY / IS_NULL / AGG use `left`)
    int height = 1;              // 1 for leaves, 1 + tallest child otherwise

    std::string ToString() const;

    // Recomputes `height` from the children; throws if the limit is exceeded.
    void FixHeight() {
        int h = 0;
        if (left) h = left->height;
        if (right && right->height > h) h = right->height;
        for (const auto& a : args)
            if (a->height > h) h = a->height;
        height = h + 1;
        if (height > kMaxExprHeight)
            throw DbError("expression is too deeply nested (more than " + std::to_string(kMaxExprHeight) + " levels)");
    }

    static ExprPtr Const(Value v) {
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::CONST;
        e->value = std::move(v);
        return e;
    }
    static ExprPtr Col(std::string table, std::string column) {
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::COLUMN;
        e->table = std::move(table);
        e->column = std::move(column);
        return e;
    }
    static ExprPtr Binary(BinOp op, ExprPtr l, ExprPtr r) {
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::BINARY;
        e->op = op;
        e->left = std::move(l);
        e->right = std::move(r);
        e->FixHeight();
        return e;
    }
    static ExprPtr Unary(UnaryOp op, ExprPtr x) {
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::UNARY;
        e->uop = op;
        e->left = std::move(x);
        e->FixHeight();
        return e;
    }
    static ExprPtr IsNull(ExprPtr x, bool negated) {
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::IS_NULL;
        e->negated = negated;
        e->left = std::move(x);
        e->FixHeight();
        return e;
    }
    static ExprPtr Agg(AggFunc f, ExprPtr arg, bool star, bool distinct = false) {
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::AGG;
        e->agg = f;
        e->agg_star = star;
        e->agg_distinct = distinct;
        e->left = std::move(arg);
        e->FixHeight();
        return e;
    }
    static ExprPtr Subquery(SubqueryKind k, std::shared_ptr<const SelectStmt> stmt, ExprPtr left = nullptr,
                            bool negated = false) {
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::SUBQUERY;
        e->sub_kind = k;
        e->subquery = std::move(stmt);
        e->left = std::move(left);
        e->negated = negated;
        e->FixHeight();
        return e;
    }
    static ExprPtr Func(std::string name, std::vector<ExprPtr> args) {
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::FUNC;
        e->func = std::move(name);
        e->args = std::move(args);
        e->FixHeight();
        return e;
    }
};

struct ColumnDef {
    std::string name;
    TypeId type = TypeId::INT;
    bool primary_key = false;
    bool not_null = false;
    bool unique = false;
    ExprPtr default_expr;  // DEFAULT <constant expression>
};

struct CreateTableStmt {
    std::string name;
    std::vector<ColumnDef> cols;
    // Table constraints: PRIMARY KEY (a, b, ...) and UNIQUE (a, b, ...). The parser
    // checks that the columns exist and marks the primary key's NOT NULL.
    std::vector<std::string> primary_key;
    std::vector<std::vector<std::string>> unique_keys;
};

struct CreateIndexStmt {
    std::string name, table;
    std::vector<std::string> columns;  // one or more, in index order
    bool unique = false;
};

struct DropStmt {
    bool index = false;
    std::string name;
};

struct SelectStmt;

struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;
    std::vector<std::vector<ExprPtr>> rows;  // VALUES (...), (...)
    std::shared_ptr<SelectStmt> select;      // or INSERT ... SELECT
};

struct SelectItem {
    ExprPtr expr;
    std::string alias;
    bool star = false;
    std::string star_table;  // for t.*
};

struct TableRef {
    std::string name, alias;
    // "... LEFT JOIN name ON on": every row of the tables before this one is kept,
    // padded with NULLs when no row of this table satisfies `on`. Inner-join ON
    // conditions are simply merged into WHERE, but a LEFT JOIN's must stay here.
    bool left_join = false;
    // "(SELECT ...) alias": a subquery in FROM (derived table) instead of a stored table.
    std::shared_ptr<SelectStmt> subquery;
    ExprPtr on;
};

struct OrderItem {
    ExprPtr expr;
    bool desc = false;
};

enum class SetOp { UNION, INTERSECT, EXCEPT };

struct SelectStmt {
    // Compound query `set_lhs <set_op> [ALL] set_rhs` (UNION / INTERSECT / EXCEPT)
    // when set_lhs is set: then only order_by / limit / offset below are used,
    // and they apply to the combined result.
    std::shared_ptr<SelectStmt> set_lhs, set_rhs;
    SetOp set_op = SetOp::UNION;
    bool set_all = false;

    bool distinct = false;
    std::vector<SelectItem> items;
    std::vector<TableRef> from;
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    ExprPtr having;
    std::vector<OrderItem> order_by;
    int64_t limit = -1;
    int64_t offset = 0;
};

struct DeleteStmt {
    std::string table;
    ExprPtr where;
};

struct UpdateStmt {
    std::string table;
    std::vector<std::pair<std::string, ExprPtr>> sets;
    ExprPtr where;
};

struct ExplainStmt {
    std::shared_ptr<SelectStmt> select;
    bool analyze = false;
};

// ALTER TABLE table ADD [COLUMN] col | DROP [COLUMN] name | RENAME [COLUMN] name TO new_name
//                  | RENAME TO new_name
struct AlterTableStmt {
    enum class Action { ADD_COLUMN, DROP_COLUMN, RENAME_COLUMN, RENAME_TABLE };
    std::string table;
    Action action = Action::ADD_COLUMN;
    ColumnDef column;      // ADD_COLUMN
    std::string name;      // DROP_COLUMN, RENAME_COLUMN: the existing column
    std::string new_name;  // RENAME_COLUMN, RENAME_TABLE
};

using Statement = std::variant<CreateTableStmt, CreateIndexStmt, DropStmt, InsertStmt, SelectStmt, DeleteStmt,
                               UpdateStmt, ExplainStmt, AlterTableStmt>;

}  // namespace minidb
