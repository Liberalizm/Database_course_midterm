// ast.h - abstract syntax tree produced by the SQL parser.
#pragma once

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "value.h"

namespace minidb {

enum class ExprKind { CONST, COLUMN, BINARY, UNARY, IS_NULL, AGG };
enum class BinOp { EQ, NE, LT, LE, GT, GE, AND, OR, ADD, SUB, MUL, DIV, MOD, LIKE };
enum class UnaryOp { NOT, NEG };
enum class AggFunc { COUNT, SUM, AVG, MIN, MAX };

struct Expr;

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
    ExprPtr left, right;         // children (UNARY / IS_NULL / AGG use `left`)
    int height = 1;              // 1 for leaves, 1 + tallest child otherwise

    std::string ToString() const;

    // Recomputes `height` from the children; throws if the limit is exceeded.
    void FixHeight() {
        int h = 0;
        if (left) h = left->height;
        if (right && right->height > h) h = right->height;
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
    static ExprPtr Agg(AggFunc f, ExprPtr arg, bool star) {
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::AGG;
        e->agg = f;
        e->agg_star = star;
        e->left = std::move(arg);
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
};

struct CreateTableStmt {
    std::string name;
    std::vector<ColumnDef> cols;
};

struct CreateIndexStmt {
    std::string name, table, column;
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
};

struct OrderItem {
    ExprPtr expr;
    bool desc = false;
};

struct SelectStmt {
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

using Statement = std::variant<CreateTableStmt, CreateIndexStmt, DropStmt, InsertStmt, SelectStmt, DeleteStmt,
                               UpdateStmt, ExplainStmt>;

}  // namespace minidb
