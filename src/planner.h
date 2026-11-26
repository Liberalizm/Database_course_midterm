// planner.h - turns a parsed SELECT into a tree of physical operators.
//
// Optimizations performed:
//   * predicate pushdown        - single-table conditions are evaluated inside the scan
//   * access path selection     - Seq Scan vs Index Scan, chosen by a simple cost model
//   * join ordering (greedy)    - start with the smallest input, prefer connected tables
//   * join algorithm selection  - Hash Join for equi-joins, Nested Loop otherwise
//   * hash join build side      - the smaller (estimated) input is hashed
#pragma once

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

    // Access path for single-table DML (DELETE / UPDATE ... WHERE ...).
    std::unique_ptr<ScanExecutor> PlanTableScan(TableInfo* table, const ExprPtr& where);

    // Replaces every subquery in an expression that is evaluated outside of a
    // SELECT (INSERT ... VALUES, UPDATE ... SET) by its result.
    ExprPtr ResolveSubqueries(const ExprPtr& e);

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
    bool ResolvesInEnclosingQuery(const Expr& col) const;

    std::unique_ptr<ScanExecutor> BuildAccessPath(const Source& src, const std::vector<ExprPtr>& conjuncts);
    ExecPtr PlanSetOp(const SelectStmt& stmt);  // UNION / INTERSECT / EXCEPT
    // Qualifies every column with its source alias; runs (uncorrelated) subqueries.
    ExprPtr Canonicalize(const ExprPtr& e, const std::vector<Source>& srcs);
    // Plans and executes the subquery of a SUBQUERY node once and returns its
    // replacement: a constant (scalar, EXISTS) or an IN_SET node. `left` is the
    // already processed left operand of IN.
    ExprPtr RunSubquery(const Expr& node, ExprPtr left, const std::vector<Source>* srcs);
    static void ReferencedTables(const Expr& e, const std::vector<Source>& srcs, std::set<int>* out);

    Catalog* catalog_;
};

}  // namespace minidb
