// planner.h - turns a parsed SELECT into a tree of physical operators.
//
// Optimizations performed:
//   * predicate pushdown        - single-table conditions are evaluated inside the scan
//   * access path selection     - Seq Scan vs Index Scan, chosen by a simple cost model
//   * join ordering (greedy)    - start with the smallest input, prefer connected tables
//   * join algorithm selection  - Hash Join for equi-joins, Nested Loop otherwise
//   * hash join build side      - the smaller (estimated) input is hashed
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "catalog.h"
#include "executors.h"

namespace minidb {

class Planner {
public:
    explicit Planner(Catalog* catalog) : catalog_(catalog) {}

    ExecPtr PlanSelect(const SelectStmt& stmt);

    // Plans the query of view `name` (renaming its output columns to `columns`
    // when given) and stores the view's output schema in *out. The query is
    // planned on its own, never seeing the columns of a query that uses the view.
    // Throws if it is invalid, if it refers to view `name` itself (directly or
    // through other views), or if two output columns share a name. CREATE VIEW
    // uses it to validate a definition; the planner to expand a view in FROM.
    ExecPtr PlanView(const std::string& name, const std::vector<std::string>& columns, const SelectStmt& query,
                     Schema* out);

    // Access path for single-table DML (DELETE / UPDATE ... WHERE ...).
    std::unique_ptr<ScanExecutor> PlanTableScan(TableInfo* table, const ExprPtr& where);

    // Binds an expression evaluated once per row of `table` (UPDATE ... SET) to
    // the table's schema. Columns may be qualified with the table name, and a
    // subquery may refer to the row being updated: it is then correlated and
    // re-run per row. Uncorrelated subqueries run once, now.
    ExprPtr PlanRowExpr(TableInfo* table, const ExprPtr& e);

    // Replaces every subquery in an expression that has no current row (INSERT
    // ... VALUES, DEFAULT) by its result; a subquery may still be correlated
    // within itself. `insert_target` is the table being inserted into, which
    // VALUES cannot refer to: naming it gets an error that says so.
    ExprPtr ResolveSubqueries(const ExprPtr& e, const TableInfo* insert_target = nullptr);

    // Renders a plan tree (EXPLAIN). `analyze` adds actual row counts.
    static std::vector<std::string> Explain(const Executor& root, bool analyze);

private:
    struct Source {
        TableInfo* table;
        std::string alias;
        Schema schema;
    };

    // While a subquery is planned, scopes_ holds one Scope per enclosing query
    // (innermost last). A column that the subquery cannot resolve itself but an
    // enclosing query can becomes a PARAM of the innermost scope; such a
    // subquery is correlated and turns into a SUBPLAN node, re-run per outer row.
    struct Scope {
        const std::vector<Source>* srcs = nullptr;
        std::shared_ptr<std::vector<Value>> params = std::make_shared<std::vector<Value>>();
        std::vector<ExprPtr> refs;           // the outer column behind each param slot
        std::map<std::string, int> slot_of;  // "table.column" as written -> slot
    };
    std::vector<Scope*> scopes_;
    const TableInfo* insert_target_ = nullptr;  // see ResolveSubqueries
    ExprPtr ResolveSubqueriesRec(const ExprPtr& e);
    bool ResolvesInEnclosingQuery(const Expr& col) const;

    // The views being expanded right now (outermost first), to detect a view
    // that reaches itself, and to bound the nesting (each level recurses).
    std::vector<std::string> view_stack_;
    static constexpr size_t kMaxViewDepth = 64;

    std::unique_ptr<ScanExecutor> BuildAccessPath(const Source& src, const std::vector<ExprPtr>& conjuncts);
    ExecPtr PlanSetOp(const SelectStmt& stmt);  // UNION / INTERSECT / EXCEPT
    // Qualifies every column with its source alias; runs (uncorrelated) subqueries.
    ExprPtr Canonicalize(const ExprPtr& e, const std::vector<Source>& srcs);
    // Plans and executes the subquery of a SUBQUERY node once and returns its
    // replacement: a constant (scalar, EXISTS) or an IN_SET node. `left` is the
    // already processed left operand of IN.
    ExprPtr RunSubquery(const Expr& node, ExprPtr left, const std::vector<Source>* srcs);
    static void ReferencedTables(const Expr& e, const std::vector<Source>& srcs, std::set<int>* out);
    // Statistics (ANALYZE) of a column of one of `srcs`, for the estimates.
    static std::function<const ColumnStats*(const Expr&)> StatsLookupFor(const std::vector<Source>* srcs);

    Catalog* catalog_;
};

}  // namespace minidb
