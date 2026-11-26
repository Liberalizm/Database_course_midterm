#include "planner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

namespace minidb {

namespace {

// ---- cost model constants (units: "one sequential page read") ----
constexpr double CPU_TUPLE_COST = 0.01;  // processing one tuple
constexpr double RANDOM_PAGE_COST = 2.0;  // random heap page fetch through an index
constexpr double HASH_TUPLE_COST = 0.02;  // building / probing a hash table

// Heuristic selectivity of a predicate (fraction of rows that pass).
double Selectivity(const Expr& e) {
    switch (e.kind) {
        case ExprKind::BINARY:
            switch (e.op) {
                case BinOp::AND: return Selectivity(*e.left) * Selectivity(*e.right);
                case BinOp::OR: {
                    double a = Selectivity(*e.left), b = Selectivity(*e.right);
                    return a + b - a * b;
                }
                case BinOp::EQ: return 0.05;
                case BinOp::NE: return 0.95;
                case BinOp::LT:
                case BinOp::LE:
                case BinOp::GT:
                case BinOp::GE: return 0.3;
                case BinOp::LIKE: return 0.1;
                default: return 0.5;
            }
        case ExprKind::UNARY: return e.uop == UnaryOp::NOT ? 1.0 - Selectivity(*e.left) : 0.5;
        case ExprKind::IS_NULL: return e.negated ? 0.95 : 0.05;
        case ExprKind::CONST: return (e.value.Type() == TypeId::BOOL && e.value.AsBool()) ? 1.0 : 0.0;
        default: return 0.5;
    }
}

double SelectivityOf(const std::vector<ExprPtr>& es) {
    double s = 1.0;
    for (const auto& e : es) s *= Selectivity(*e);
    return s;
}

BinOp Flip(BinOp op) {
    switch (op) {
        case BinOp::LT: return BinOp::GT;
        case BinOp::LE: return BinOp::GE;
        case BinOp::GT: return BinOp::LT;
        case BinOp::GE: return BinOp::LE;
        default: return op;
    }
}

// Does `e` have the shape  alias.col <op> <constant>  (either order) usable by `idx`?
// INT index: any comparison with an INT constant. Hashed TEXT index: only
// equality with a TEXT constant, and `v` is the hash of that constant.
bool MatchIndexPredicate(const Expr& e, const std::string& alias, const IndexInfo& idx, BinOp* op, int64_t* v) {
    if (e.kind != ExprKind::BINARY) return false;
    BinOp o = e.op;
    if (o != BinOp::EQ && o != BinOp::LT && o != BinOp::LE && o != BinOp::GT && o != BinOp::GE) return false;
    const Expr* c = e.left.get();
    const Expr* k = e.right.get();
    if (c->kind != ExprKind::COLUMN) {
        std::swap(c, k);
        o = Flip(o);
    }
    if (c->kind != ExprKind::COLUMN || k->kind != ExprKind::CONST) return false;
    if (c->table != alias || c->column != idx.column) return false;
    if (idx.hashed) {
        if (o != BinOp::EQ || k->value.Type() != TypeId::TEXT) return false;
        *op = o;
        *v = HashTextKey(k->value.AsText());
        return true;
    }
    if (k->value.Type() != TypeId::INT) return false;
    *op = o;
    *v = k->value.AsInt();
    return true;
}

void TightenLo(std::optional<IndexBound>& lo, int64_t v, bool incl) {
    if (!lo || v > lo->value || (v == lo->value && !incl)) lo = IndexBound{v, incl};
}
void TightenHi(std::optional<IndexBound>& hi, int64_t v, bool incl) {
    if (!hi || v < hi->value || (v == hi->value && !incl)) hi = IndexBound{v, incl};
}

bool IsSubset(const std::set<int>& a, const std::set<int>& b) {
    return std::includes(b.begin(), b.end(), a.begin(), a.end());
}

// Column reference already bound to a fixed position (used after aggregation).
ExprPtr PreboundColumn(int idx, const std::string& name) {
    auto e = Expr::Col("", name);
    e->col_idx = idx;
    e->prebound = true;
    return e;
}

}  // namespace

bool Planner::ResolvesInEnclosingQuery(const Expr& col) const {
    for (const Scope* s : scopes_) {
        for (const auto& src : *s->srcs) {
            if (!col.table.empty() && src.alias != col.table) continue;
            if (src.schema.Find("", col.column) >= 0) return true;
        }
    }
    return false;
}

ExprPtr Planner::RunSubquery(const Expr& node, ExprPtr left, const std::vector<Source>* srcs) {
    // Plan the subquery with the enclosing query's sources in scope (when there is
    // one), so references to outer columns become PARAMs instead of errors.
    Scope scope;
    scope.srcs = srcs;
    struct ScopeGuard {
        std::vector<Scope*>* stack;
        bool pushed;
        ~ScopeGuard() {
            if (pushed) stack->pop_back();
        }
    };
    if (srcs) scopes_.push_back(&scope);
    ScopeGuard guard{&scopes_, srcs != nullptr};
    ExecPtr plan = PlanSelect(*node.subquery);
    if (node.sub_kind != SubqueryKind::EXISTS && plan->OutSchema().Size() != 1)
        throw DbError("subquery must return only one column");

    if (!scope.refs.empty()) {
        // Correlated: keep the plan and run it again for every outer row.
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::SUBPLAN;
        e->sub_kind = node.sub_kind;
        e->negated = node.negated;
        e->left = std::move(left);
        e->subplan = std::shared_ptr<Executor>(std::move(plan));
        e->params = scope.params;
        e->args = scope.refs;  // canonicalized by the caller, in the outer query's scope
        e->FixHeight();
        return e;
    }

    // Uncorrelated: run it once, now, and replace it by its result.
    plan->Init();
    Row r;
    switch (node.sub_kind) {
        case SubqueryKind::EXISTS: return Expr::Const(Value::Bool(plan->Next(&r) != node.negated));
        case SubqueryKind::SCALAR: {
            if (!plan->Next(&r)) return Expr::Const(Value::Null());
            Value v = r[0];
            if (plan->Next(&r)) throw DbError("more than one row returned by a subquery used as an expression");
            return Expr::Const(std::move(v));
        }
        case SubqueryKind::IN: {
            auto set = std::make_shared<std::unordered_set<std::string>>();
            auto e = std::make_shared<Expr>();
            e->kind = ExprKind::IN_SET;
            e->negated = node.negated;
            e->left = std::move(left);
            while (plan->Next(&r)) {
                if (r[0].IsNull()) {
                    e->in_set_has_null = true;
                    continue;
                }
                std::string k;
                r[0].AppendKey(&k);  // the same equality as "=" (1 = 1.0)
                set->insert(std::move(k));
            }
            e->in_set = std::move(set);
            e->FixHeight();
            return e;
        }
    }
    throw DbError("internal: bad subquery kind");
}

ExprPtr Planner::ResolveSubqueries(const ExprPtr& e, const TableInfo* insert_target) {
    struct Restore {
        const TableInfo** slot;
        const TableInfo* saved;
        ~Restore() { *slot = saved; }
    } restore{&insert_target_, insert_target_};
    insert_target_ = insert_target;
    return ResolveSubqueriesRec(e);
}

ExprPtr Planner::ResolveSubqueriesRec(const ExprPtr& e) {
    if (!e) return nullptr;
    if (e->kind == ExprKind::SUBQUERY) return RunSubquery(*e, ResolveSubqueriesRec(e->left), nullptr);
    auto c = std::make_shared<Expr>(*e);
    c->left = ResolveSubqueriesRec(e->left);
    c->right = ResolveSubqueriesRec(e->right);
    for (auto& a : c->args) a = ResolveSubqueriesRec(a);
    return c;
}

ExprPtr Planner::PlanRowExpr(TableInfo* table, const ExprPtr& e) {
    // The same scope as the statement's WHERE (PlanTableScan): the target table,
    // under its own name, so a subquery's reference to it becomes a PARAM.
    std::vector<Source> srcs{{table, table->name, table->schema.WithQualifier(table->name)}};
    return BindExpr(Canonicalize(e, srcs), srcs[0].schema);
}

ExprPtr Planner::Canonicalize(const ExprPtr& e, const std::vector<Source>& srcs) {
    if (!e) return nullptr;
    if (e->kind == ExprKind::SUBQUERY) {
        ExprPtr r = RunSubquery(*e, Canonicalize(e->left, srcs), &srcs);
        // The outer columns a correlated subquery reads belong to this query (or,
        // when nested deeper, to one around it: then they become params here too).
        if (r->kind == ExprKind::SUBPLAN)
            for (auto& a : r->args) a = Canonicalize(a, srcs);
        return r;
    }
    auto c = std::make_shared<Expr>(*e);
    if (e->kind == ExprKind::COLUMN) {
        if (e->prebound) return c;
        int found = -1;
        bool alias_exists = false;
        for (size_t i = 0; i < srcs.size(); ++i) {
            if (!e->table.empty() && srcs[i].alias != e->table) continue;
            alias_exists = true;
            if (srcs[i].schema.Find("", e->column) >= 0) {
                if (found >= 0) throw DbError("column reference \"" + e->column + "\" is ambiguous");
                found = static_cast<int>(i);
            }
        }
        if (found < 0 && !scopes_.empty() && ResolvesInEnclosingQuery(*e)) {
            // A column of an enclosing query: a parameter of the innermost scope.
            Scope& s = *scopes_.back();
            auto [it, inserted] = s.slot_of.emplace(e->table + "." + e->column, static_cast<int>(s.refs.size()));
            if (inserted) {
                s.refs.push_back(Expr::Col(e->table, e->column));
                s.params->push_back(Value::Null());
            }
            auto p = std::make_shared<Expr>();
            p->kind = ExprKind::PARAM;
            p->params = s.params;
            p->param_slot = it->second;
            p->table = e->table;
            p->column = e->column;
            return p;
        }
        if (found < 0) {
            for (const Scope* s : scopes_)  // e.g. "emp.nope" where emp is an outer table
                for (const auto& src : *s->srcs) alias_exists = alias_exists || src.alias == e->table;
            if (const TableInfo* t = insert_target_) {  // in a subquery of INSERT ... VALUES
                if (!alias_exists && e->table == t->name)
                    throw DbError("invalid reference to table \"" + t->name +
                                  "\": INSERT ... VALUES has no current row to refer to");
                if (e->table.empty() && t->schema.Find("", e->column) >= 0)
                    throw DbError("column \"" + e->column + "\" does not exist (table \"" + t->name +
                                  "\" has one, but INSERT ... VALUES cannot refer to it)");
            }
            if (!e->table.empty() && !alias_exists)
                throw DbError("missing FROM-clause entry for table \"" + e->table + "\"");
            throw DbError("column \"" + e->ToString() + "\" does not exist");
        }
        c->table = srcs[static_cast<size_t>(found)].alias;
        return c;
    }
    c->left = Canonicalize(e->left, srcs);
    c->right = Canonicalize(e->right, srcs);
    for (auto& a : c->args) a = Canonicalize(a, srcs);
    return c;
}

void Planner::ReferencedTables(const Expr& e, const std::vector<Source>& srcs, std::set<int>* out) {
    if (e.kind == ExprKind::COLUMN && !e.prebound) {
        for (size_t i = 0; i < srcs.size(); ++i)
            if (srcs[i].alias == e.table) out->insert(static_cast<int>(i));
    }
    if (e.left) ReferencedTables(*e.left, srcs, out);
    if (e.right) ReferencedTables(*e.right, srcs, out);
    for (const auto& a : e.args) ReferencedTables(*a, srcs, out);
}

std::unique_ptr<ScanExecutor> Planner::BuildAccessPath(const Source& src, const std::vector<ExprPtr>& conjuncts) {
    TableInfo* t = src.table;
    const double rows = std::max(1.0, static_cast<double>(t->row_count));
    const double pages = std::max(1.0, static_cast<double>(t->heap->PageCount()));

    // Option 1: full sequential scan.
    double best_cost = pages + rows * CPU_TUPLE_COST;
    double best_rows = rows * SelectivityOf(conjuncts);
    IndexInfo* best_idx = nullptr;
    std::optional<IndexBound> best_lo, best_hi;
    std::vector<size_t> best_used;

    // Option 2..n: an index scan for every usable index.
    for (IndexInfo* idx : catalog_->IndexesOf(t->name)) {
        std::optional<IndexBound> lo, hi;
        std::optional<size_t> eq_conj;
        int64_t eq_val = 0;
        std::vector<size_t> range_used;
        for (size_t i = 0; i < conjuncts.size(); ++i) {
            BinOp op;
            int64_t v;
            if (!MatchIndexPredicate(*conjuncts[i], src.alias, *idx, &op, &v)) continue;
            switch (op) {
                case BinOp::EQ:
                    if (!eq_conj) {
                        eq_conj = i;
                        eq_val = v;
                    }
                    break;
                case BinOp::GT: TightenLo(lo, v, false); range_used.push_back(i); break;
                case BinOp::GE: TightenLo(lo, v, true); range_used.push_back(i); break;
                case BinOp::LT: TightenHi(hi, v, false); range_used.push_back(i); break;
                case BinOp::LE: TightenHi(hi, v, true); range_used.push_back(i); break;
                default: break;
            }
        }
        // Statistics: min/max of the column straight from the B+ tree. Assume
        // values are uniformly distributed in [min, max].
        int64_t kmin = 0, kmax = 0;
        // (Not for a hashed index: the min/max of hash values says nothing.)
        bool have_stats = !idx->hashed && idx->tree->MinKey(&kmin) && idx->tree->MaxKey(&kmax) && kmax >= kmin;
        double domain = have_stats ? static_cast<double>(kmax) - static_cast<double>(kmin) + 1.0 : 0.0;

        double matched;
        std::vector<size_t> used;
        if (eq_conj) {
            lo = hi = IndexBound{eq_val, true};
            used = {*eq_conj};
            if (idx->unique)
                matched = 1.0;
            else if (have_stats)
                matched = (eq_val < kmin || eq_val > kmax) ? 1.0 : std::max(1.0, rows / domain);
            else
                matched = std::max(1.0, rows * 0.05);
        } else if (lo || hi) {
            used = range_used;
            if (have_stats) {
                double a = lo ? static_cast<double>(lo->value) + (lo->inclusive ? 0 : 1) : static_cast<double>(kmin);
                double b = hi ? static_cast<double>(hi->value) - (hi->inclusive ? 0 : 1) : static_cast<double>(kmax);
                a = std::max(a, static_cast<double>(kmin));
                b = std::min(b, static_cast<double>(kmax));
                double frac = b < a ? 0.0 : (b - a + 1.0) / domain;
                matched = std::max(1.0, rows * frac);
            } else {
                matched = std::max(1.0, rows * ((lo && hi) ? 0.1 : 0.3));
            }
        } else {
            continue;
        }
        std::vector<ExprPtr> rest;
        for (size_t i = 0; i < conjuncts.size(); ++i)
            if (std::find(used.begin(), used.end(), i) == used.end()) rest.push_back(conjuncts[i]);

        // Cardenas' formula: expected number of distinct heap pages touched when
        // fetching `matched` random rows out of `pages` pages.
        double pages_fetched = pages * (1.0 - std::pow(1.0 - 1.0 / pages, matched));
        if (pages <= 1.0) pages_fetched = 1.0;
        double cost = idx->tree->Height() + matched / BPlusTree::LEAF_CAPACITY + pages_fetched * RANDOM_PAGE_COST +
                      matched * CPU_TUPLE_COST;
        if (cost < best_cost) {
            best_cost = cost;
            best_idx = idx;
            best_lo = lo;
            best_hi = hi;
            best_used = used;
            best_rows = matched * SelectivityOf(rest);
        }
    }

    // Conditions answered by the index are dropped from the filter, except for a
    // hashed (TEXT) index: two texts can share a hash, so `col = 'x'` is re-checked.
    const bool recheck = best_idx && best_idx->hashed;
    std::vector<ExprPtr> residual;
    for (size_t i = 0; i < conjuncts.size(); ++i)
        if (recheck || std::find(best_used.begin(), best_used.end(), i) == best_used.end())
            residual.push_back(conjuncts[i]);
    ExprPtr pred = MakeConjunction(residual);
    ExprPtr bound = pred ? BindExpr(pred, src.schema) : nullptr;

    std::unique_ptr<ScanExecutor> scan;
    if (best_idx)
        scan = std::make_unique<IndexScanExecutor>(t, best_idx, src.alias, src.schema, best_lo, best_hi, bound);
    else
        scan = std::make_unique<SeqScanExecutor>(t, src.alias, src.schema, bound);
    scan->est_rows = std::max(1.0, best_rows);
    scan->est_cost = best_cost;
    return scan;
}

std::unique_ptr<ScanExecutor> Planner::PlanTableScan(TableInfo* table, const ExprPtr& where) {
    std::vector<Source> srcs{{table, table->name, table->schema.WithQualifier(table->name)}};
    ExprPtr canon = Canonicalize(where, srcs);
    if (canon && ContainsAggregate(*canon)) throw DbError("aggregate functions are not allowed in WHERE");
    return BuildAccessPath(srcs[0], SplitConjuncts(canon));
}

ExecPtr Planner::PlanSetOp(const SelectStmt& stmt) {
    ExecPtr l = PlanSelect(*stmt.set_lhs);
    ExecPtr r = PlanSelect(*stmt.set_rhs);
    static const char* names[] = {"UNION", "INTERSECT", "EXCEPT"};
    const char* name = names[static_cast<int>(stmt.set_op)];
    if (l->OutSchema().Size() != r->OutSchema().Size())
        throw DbError(std::string("each ") + name + " query must have the same number of columns");
    double lr = l->est_rows, rr = r->est_rows;
    double cost = l->est_cost + r->est_cost + (lr + rr) * HASH_TUPLE_COST;
    ExecPtr root = std::make_unique<SetOpExecutor>(std::move(l), std::move(r), stmt.set_op, stmt.set_all);
    root->est_rows = stmt.set_op == SetOp::UNION ? lr + rr : stmt.set_op == SetOp::INTERSECT ? std::min(lr, rr) : lr;
    root->est_cost = cost;

    // ORDER BY over the combined result: output column names (of the first
    // query) or positions, as in PostgreSQL.
    if (!stmt.order_by.empty()) {
        const Schema& out = root->OutSchema();
        std::vector<std::pair<ExprPtr, bool>> keys;
        for (const auto& o : stmt.order_by) {
            int idx = -1;
            const Expr& e = *o.expr;
            if (e.kind == ExprKind::CONST && e.value.Type() == TypeId::INT) {
                int64_t k = e.value.AsInt();
                if (k >= 1 && k <= static_cast<int64_t>(out.Size())) idx = static_cast<int>(k - 1);
            } else if (e.kind == ExprKind::COLUMN && e.table.empty()) {
                for (size_t i = 0; i < out.Size(); ++i) {
                    if (out.At(i).name != e.column) continue;
                    if (idx >= 0) throw DbError("ORDER BY \"" + e.column + "\" is ambiguous");
                    idx = static_cast<int>(i);
                }
            }
            if (idx < 0)
                throw DbError(std::string("ORDER BY on a ") + name +
                              " result must name an output column or position, not " + e.ToString());
            keys.emplace_back(PreboundColumn(idx, out.At(static_cast<size_t>(idx)).name), o.desc);
        }
        double est = root->est_rows, c = root->est_cost;
        root = std::make_unique<SortExecutor>(std::move(root), keys);
        root->est_rows = est;
        root->est_cost = c + est * CPU_TUPLE_COST * 2;
    }
    if (stmt.limit >= 0 || stmt.offset > 0) {
        double est = root->est_rows, c = root->est_cost;
        root = std::make_unique<LimitExecutor>(std::move(root), stmt.limit, stmt.offset);
        root->est_rows = stmt.limit >= 0 ? std::min(est, static_cast<double>(stmt.limit)) : est;
        root->est_cost = c;
    }
    return root;
}

ExecPtr Planner::PlanSelect(const SelectStmt& stmt) {
    if (stmt.set_lhs) return PlanSetOp(stmt);
    // ---------- 1. resolve FROM ----------
    std::vector<Source> srcs;
    std::vector<ExecPtr> derived;  // plan of each subquery in FROM (null for stored tables)
    for (const auto& ref : stmt.from) {
        std::string alias = ref.alias.empty() ? ref.name : ref.alias;
        for (const auto& s : srcs)
            if (s.alias == alias) throw DbError("table name \"" + alias + "\" specified more than once");
        if (ref.subquery) {
            // Derived table: planned (and later run) on its own; uncorrelated like
            // every subquery here. Its output columns become alias.column.
            ExecPtr plan = PlanSelect(*ref.subquery);
            const Schema& out = plan->OutSchema();
            for (size_t i = 0; i < out.Size(); ++i)
                for (size_t j = 0; j < i; ++j)
                    if (out.At(i).name == out.At(j).name)
                        throw DbError("subquery \"" + alias + "\" has more than one column named \"" + out.At(i).name +
                                      "\" (give them different aliases)");
            srcs.push_back({nullptr, alias, out.WithQualifier(alias)});
            derived.push_back(std::move(plan));
            continue;
        }
        TableInfo* t = catalog_->RequireTable(ref.name);
        srcs.push_back({t, alias, t->schema.WithQualifier(alias)});
        derived.push_back(nullptr);
    }

    // ---------- 2. ORDER BY may refer to select aliases or positions ----------
    std::vector<OrderItem> order = stmt.order_by;
    for (auto& o : order) {
        if (o.expr->kind == ExprKind::COLUMN && o.expr->table.empty()) {
            for (const auto& it : stmt.items) {
                if (!it.star && it.alias == o.expr->column) {
                    o.expr = it.expr;
                    break;
                }
            }
        } else if (o.expr->kind == ExprKind::CONST && o.expr->value.Type() == TypeId::INT) {
            int64_t k = o.expr->value.AsInt();
            if (k < 1 || k > static_cast<int64_t>(stmt.items.size()) || stmt.items[static_cast<size_t>(k - 1)].star)
                throw DbError("ORDER BY position " + std::to_string(k) + " is not in select list");
            o.expr = stmt.items[static_cast<size_t>(k - 1)].expr;
        }
    }

    // ---------- 3. canonicalize all expressions (qualify every column) ----------
    ExprPtr where = Canonicalize(stmt.where, srcs);
    if (where && ContainsAggregate(*where)) throw DbError("aggregate functions are not allowed in WHERE");
    std::vector<ExprPtr> group_by;
    for (const auto& g : stmt.group_by) group_by.push_back(Canonicalize(g, srcs));
    ExprPtr having = Canonicalize(stmt.having, srcs);
    for (auto& o : order) o.expr = Canonicalize(o.expr, srcs);
    std::vector<ExprPtr> item_exprs(stmt.items.size());
    for (size_t i = 0; i < stmt.items.size(); ++i)
        if (!stmt.items[i].star) item_exprs[i] = Canonicalize(stmt.items[i].expr, srcs);

    // ---------- 4. classify WHERE conjuncts by the tables they touch ----------
    // A table on the right of a LEFT JOIN is "nullable": its columns are NULL in
    // padded rows, so WHERE conditions on it must run after that join, never be
    // pushed into its scan (that would turn the LEFT JOIN into an inner join).
    bool has_left_join = false;
    std::vector<bool> nullable(srcs.size(), false);
    for (size_t i = 0; i < stmt.from.size(); ++i)
        if (stmt.from[i].left_join) has_left_join = nullable[i] = true;

    struct Pending {
        ExprPtr e;
        std::set<int> tabs;
        bool used = false;
    };
    std::vector<Pending> pending;
    std::vector<std::vector<ExprPtr>> per_table(srcs.size());
    for (auto& c : SplitConjuncts(where)) {
        std::set<int> tabs;
        ReferencedTables(*c, srcs, &tabs);
        if (tabs.size() == 1 && !nullable[static_cast<size_t>(*tabs.begin())])
            per_table[static_cast<size_t>(*tabs.begin())].push_back(c);  // pushdown
        else
            pending.push_back({c, tabs});
    }

    // LEFT JOIN ... ON: conditions on the joined table alone only decide which of
    // its rows can match, so they may filter its scan; the rest stay join conditions.
    // An ON clause may only refer to tables up to and including its own.
    std::vector<std::vector<ExprPtr>> on_conds(srcs.size());
    for (size_t i = 0; i < stmt.from.size(); ++i) {
        if (!stmt.from[i].left_join) continue;
        std::vector<Source> visible(srcs.begin(), srcs.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        ExprPtr on = Canonicalize(stmt.from[i].on, visible);
        if (ContainsAggregate(*on)) throw DbError("aggregate functions are not allowed in JOIN conditions");
        for (auto& c : SplitConjuncts(on)) {
            std::set<int> tabs;
            ReferencedTables(*c, srcs, &tabs);
            if (tabs == std::set<int>{static_cast<int>(i)})
                per_table[i].push_back(c);
            else
                on_conds[i].push_back(c);
        }
    }

    // ---------- 5. access paths ----------
    std::vector<ExecPtr> base(srcs.size());
    for (size_t i = 0; i < srcs.size(); ++i) {
        if (srcs[i].table) {
            base[i] = BuildAccessPath(srcs[i], per_table[i]);
            continue;
        }
        double est = derived[i]->est_rows, cost = derived[i]->est_cost;
        ExecPtr node = std::make_unique<SubqueryScanExecutor>(std::move(derived[i]), srcs[i].schema, srcs[i].alias);
        node->est_rows = est;
        node->est_cost = cost;
        if (!per_table[i].empty()) {  // conditions on the subquery alone filter its rows
            ExprPtr pred = BindExpr(MakeConjunction(per_table[i]), node->OutSchema());
            node = std::make_unique<FilterExecutor>(std::move(node), pred);
            node->est_rows = std::max(1.0, est * SelectivityOf(per_table[i]));
            node->est_cost = cost + est * CPU_TUPLE_COST;
        }
        base[i] = std::move(node);
    }

    // ---------- 6. join ordering + join algorithm ----------
    ExecPtr root;
    std::set<int> joined;
    auto apply_ready_filters = [&](ExecPtr node) {
        std::vector<ExprPtr> ready;
        for (auto& p : pending) {
            if (!p.used && IsSubset(p.tabs, joined)) {
                ready.push_back(p.e);
                p.used = true;
            }
        }
        if (ready.empty()) return node;
        double est = node->est_rows * SelectivityOf(ready), cost = node->est_cost;
        ExprPtr pred = BindExpr(MakeConjunction(ready), node->OutSchema());
        auto f = std::make_unique<FilterExecutor>(std::move(node), pred);
        f->est_rows = std::max(1.0, est);
        f->est_cost = cost;
        return ExecPtr(std::move(f));
    };

    // Joins `left` (the tables in `joined`) with `right` (table `next`) on `conds`:
    // a hash join if there are equi-join keys, a nested loop join otherwise.
    auto make_join = [&](ExecPtr left, ExecPtr right, int next, const std::vector<ExprPtr>& conds,
                         bool left_outer) -> ExecPtr {
        // Split into equi-join keys (left_expr = right_expr) and residual predicates.
        std::vector<ExprPtr> lkeys, rkeys, residual;
        std::vector<std::string> cond_text;
        for (auto& e : conds) {
            bool is_key = false;
            if (e->kind == ExprKind::BINARY && e->op == BinOp::EQ) {
                std::set<int> lt, rt;
                ReferencedTables(*e->left, srcs, &lt);
                ReferencedTables(*e->right, srcs, &rt);
                std::set<int> only_next{next};
                if (!lt.empty() && !rt.empty()) {
                    if (IsSubset(lt, joined) && rt == only_next) {
                        lkeys.push_back(e->left);
                        rkeys.push_back(e->right);
                        is_key = true;
                    } else if (IsSubset(rt, joined) && lt == only_next) {
                        lkeys.push_back(e->right);
                        rkeys.push_back(e->left);
                        is_key = true;
                    }
                }
            }
            if (is_key)
                cond_text.push_back(e->ToString());
            else
                residual.push_back(e);
        }

        double l_rows = left->est_rows, r_rows = right->est_rows;
        double l_cost = left->est_cost, r_cost = right->est_cost;
        ExecPtr join;
        double out_rows, cost;
        if (!lkeys.empty()) {
            // Hash join; the smaller input becomes the build (hashed) side. For a
            // LEFT JOIN the preserved left side must be the probe side.
            out_rows = l_rows * r_rows / std::max(l_rows, r_rows) * SelectivityOf(residual);
            cost = l_cost + r_cost + (l_rows + r_rows) * HASH_TUPLE_COST;
            std::string text;
            for (size_t i = 0; i < cond_text.size(); ++i) text += (i ? " AND " : "") + cond_text[i];
            bool build_left = !left_outer && l_rows < r_rows;
            ExecPtr probe = build_left ? std::move(right) : std::move(left);
            ExecPtr build = build_left ? std::move(left) : std::move(right);
            std::vector<ExprPtr> pk, bk;
            for (size_t i = 0; i < lkeys.size(); ++i) {
                const ExprPtr& pe = build_left ? rkeys[i] : lkeys[i];
                const ExprPtr& be = build_left ? lkeys[i] : rkeys[i];
                pk.push_back(BindExpr(pe, probe->OutSchema()));
                bk.push_back(BindExpr(be, build->OutSchema()));
            }
            Schema combined = Schema::Concat(probe->OutSchema(), build->OutSchema());
            ExprPtr res = residual.empty() ? nullptr : BindExpr(MakeConjunction(residual), combined);
            join = std::make_unique<HashJoinExecutor>(std::move(probe), std::move(build), pk, bk, res, text,
                                                      left_outer);
        } else {
            out_rows = l_rows * r_rows * SelectivityOf(residual);
            cost = l_cost + l_rows * r_cost + l_rows * r_rows * CPU_TUPLE_COST;
            Schema combined = Schema::Concat(left->OutSchema(), right->OutSchema());
            ExprPtr pred = residual.empty() ? nullptr : BindExpr(MakeConjunction(residual), combined);
            join = std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right), pred, left_outer);
        }
        if (left_outer) out_rows = std::max(out_rows, l_rows);  // every left row survives
        join->est_rows = std::max(1.0, out_rows);
        join->est_cost = cost;
        return join;
    };

    // Predicates from WHERE / inner ON that can become join conditions once `next`
    // is joined. Conditions on a nullable table never do: they must filter after
    // the LEFT JOIN that pads it (apply_ready_filters takes care of them).
    auto take_applicable = [&](int next) {
        std::set<int> with = joined;
        with.insert(next);
        std::vector<ExprPtr> applicable;
        for (auto& p : pending) {
            if (p.used || !IsSubset(p.tabs, with)) continue;
            bool touches_nullable = false;
            for (int t : p.tabs) touches_nullable = touches_nullable || nullable[static_cast<size_t>(t)];
            if (touches_nullable) continue;
            applicable.push_back(p.e);
            p.used = true;
        }
        return applicable;
    };

    if (srcs.empty()) {
        root = std::make_unique<ValuesExecutor>();
        root = apply_ready_filters(std::move(root));
    } else if (has_left_join) {
        // Outer joins are not freely reorderable: join in the order written.
        root = std::move(base[0]);
        joined.insert(0);
        root = apply_ready_filters(std::move(root));
        for (size_t i = 1; i < srcs.size(); ++i) {
            int next = static_cast<int>(i);
            bool left_outer = stmt.from[i].left_join;
            std::vector<ExprPtr> conds = left_outer ? on_conds[i] : take_applicable(next);
            root = make_join(std::move(root), std::move(base[i]), next, conds, left_outer);
            joined.insert(next);
            root = apply_ready_filters(std::move(root));
        }
    } else {
        int start = 0;
        for (size_t i = 1; i < srcs.size(); ++i)
            if (base[i]->est_rows < base[static_cast<size_t>(start)]->est_rows) start = static_cast<int>(i);
        root = std::move(base[static_cast<size_t>(start)]);
        joined.insert(start);
        root = apply_ready_filters(std::move(root));

        while (joined.size() < srcs.size()) {
            // Prefer a table connected to the current set by a join predicate.
            int next = -1;
            bool next_connected = false;
            for (size_t t = 0; t < srcs.size(); ++t) {
                if (joined.count(static_cast<int>(t))) continue;
                std::set<int> with = joined;
                with.insert(static_cast<int>(t));
                bool connected = false;
                for (auto& p : pending)
                    if (!p.used && p.tabs.count(static_cast<int>(t)) && IsSubset(p.tabs, with)) connected = true;
                bool better = next < 0 || (connected && !next_connected) ||
                              (connected == next_connected && base[t]->est_rows < base[static_cast<size_t>(next)]->est_rows);
                if (better) {
                    next = static_cast<int>(t);
                    next_connected = connected;
                }
            }
            std::vector<ExprPtr> applicable = take_applicable(next);
            root = make_join(std::move(root), std::move(base[static_cast<size_t>(next)]), next, applicable, false);
            joined.insert(next);
            root = apply_ready_filters(std::move(root));
        }
    }

    // ---------- 7. aggregation ----------
    bool has_agg = !group_by.empty() || (having && ContainsAggregate(*having));
    for (size_t i = 0; i < stmt.items.size(); ++i)
        if (item_exprs[i] && ContainsAggregate(*item_exprs[i])) has_agg = true;
    for (const auto& o : order)
        if (ContainsAggregate(*o.expr)) has_agg = true;

    // After aggregation, expressions must be rewritten to reference the
    // aggregate's output columns: [group exprs..., aggregates...].
    std::vector<std::string> group_keys;
    std::map<std::string, int> agg_index;
    std::vector<AggSpec> aggs;
    std::function<void(const ExprPtr&)> collect = [&](const ExprPtr& e) {
        if (!e) return;
        if (e->kind == ExprKind::AGG) {
            std::string k = e->ToString();
            if (!agg_index.count(k)) {
                if (e->left && ContainsAggregate(*e->left)) throw DbError("aggregate calls cannot be nested");
                agg_index[k] = static_cast<int>(aggs.size());
                aggs.push_back({e->agg, e->agg_star ? nullptr : BindExpr(e->left, root->OutSchema()), k, e->agg_distinct});
            }
            return;
        }
        collect(e->left);
        collect(e->right);
        for (const auto& a : e->args) collect(a);
    };
    std::function<ExprPtr(const ExprPtr&)> rewrite = [&](const ExprPtr& e) -> ExprPtr {
        if (!e) return nullptr;
        std::string s = e->ToString();
        for (size_t i = 0; i < group_keys.size(); ++i)
            if (group_keys[i] == s) return PreboundColumn(static_cast<int>(i), s);
        if (e->kind == ExprKind::AGG)
            return PreboundColumn(static_cast<int>(group_keys.size()) + agg_index.at(s), s);
        if (e->kind == ExprKind::COLUMN)
            throw DbError("column \"" + s + "\" must appear in the GROUP BY clause or be used in an aggregate function");
        auto c = std::make_shared<Expr>(*e);
        c->left = rewrite(e->left);
        c->right = rewrite(e->right);
        for (auto& a : c->args) a = rewrite(a);
        return c;
    };

    if (has_agg) {
        for (size_t i = 0; i < stmt.items.size(); ++i) {
            if (stmt.items[i].star) throw DbError("SELECT * is not allowed together with GROUP BY / aggregates");
            collect(item_exprs[i]);
        }
        collect(having);
        for (const auto& o : order) collect(o.expr);
        std::vector<ExprPtr> bound_groups;
        for (const auto& g : group_by) {
            group_keys.push_back(g->ToString());
            bound_groups.push_back(BindExpr(g, root->OutSchema()));
        }
        double in_rows = root->est_rows, in_cost = root->est_cost;
        auto agg = std::make_unique<AggregateExecutor>(std::move(root), bound_groups, group_keys, aggs);
        agg->est_rows = group_by.empty() ? 1.0 : std::max(1.0, in_rows * 0.1);
        agg->est_cost = in_cost + in_rows * HASH_TUPLE_COST;
        root = std::move(agg);
        if (having) {
            double est = root->est_rows * 0.5, cost = root->est_cost;
            ExprPtr pred = BindExpr(rewrite(having), root->OutSchema());
            root = std::make_unique<FilterExecutor>(std::move(root), pred, "Having");
            root->est_rows = std::max(1.0, est);
            root->est_cost = cost;
        }
    } else if (having) {
        throw DbError("HAVING requires GROUP BY or aggregate functions");
    }

    // ---------- 8. ORDER BY ----------
    if (!order.empty()) {
        std::vector<std::pair<ExprPtr, bool>> keys;
        for (const auto& o : order) {
            ExprPtr e = has_agg ? rewrite(o.expr) : o.expr;
            keys.emplace_back(BindExpr(e, root->OutSchema()), o.desc);
        }
        double est = root->est_rows, cost = root->est_cost;
        root = std::make_unique<SortExecutor>(std::move(root), keys);
        root->est_rows = est;
        root->est_cost = cost + est * CPU_TUPLE_COST * 2;
    }

    // ---------- 9. projection ----------
    std::vector<ExprPtr> proj;
    std::vector<std::string> names;
    for (size_t i = 0; i < stmt.items.size(); ++i) {
        const SelectItem& it = stmt.items[i];
        if (it.star) {
            bool any = false;
            for (const auto& s : srcs) {
                if (!it.star_table.empty() && s.alias != it.star_table) continue;
                any = true;
                for (const auto& c : s.schema.Columns()) {
                    proj.push_back(BindExpr(Expr::Col(s.alias, c.name), root->OutSchema()));
                    names.push_back(c.name);
                }
            }
            if (!any) {
                if (srcs.empty()) throw DbError("SELECT * with no tables specified is not valid");
                throw DbError("missing FROM-clause entry for table \"" + it.star_table + "\"");
            }
            continue;
        }
        ExprPtr e = has_agg ? rewrite(item_exprs[i]) : item_exprs[i];
        proj.push_back(BindExpr(e, root->OutSchema()));
        if (!it.alias.empty())
            names.push_back(it.alias);
        else if (it.expr->kind == ExprKind::COLUMN)
            names.push_back(it.expr->column);
        else
            names.push_back(it.expr->ToString());
    }
    {
        double est = root->est_rows, cost = root->est_cost;
        root = std::make_unique<ProjectionExecutor>(std::move(root), proj, names);
        root->est_rows = est;
        root->est_cost = cost;
    }

    // ---------- 10. DISTINCT, LIMIT ----------
    if (stmt.distinct) {
        double est = root->est_rows, cost = root->est_cost;
        root = std::make_unique<DistinctExecutor>(std::move(root));
        root->est_rows = est;
        root->est_cost = cost + est * HASH_TUPLE_COST;
    }
    if (stmt.limit >= 0 || stmt.offset > 0) {
        double est = root->est_rows, cost = root->est_cost;
        root = std::make_unique<LimitExecutor>(std::move(root), stmt.limit, stmt.offset);
        root->est_rows = stmt.limit >= 0 ? std::min(est, static_cast<double>(stmt.limit)) : est;
        root->est_cost = cost;
    }
    return root;
}

std::vector<std::string> Planner::Explain(const Executor& root, bool analyze) {
    std::vector<std::string> out;
    std::function<void(const Executor&, int)> walk = [&](const Executor& e, int depth) {
        std::string line(static_cast<size_t>(depth) * 4, ' ');
        if (depth > 0) line += "-> ";
        line += e.Describe();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  (cost=%.2f rows=%.0f)", e.est_cost, e.est_rows);
        line += buf;
        if (analyze) {
            std::snprintf(buf, sizeof(buf), " (actual rows=%llu loops=%llu)",
                          static_cast<unsigned long long>(e.ActualRows()), static_cast<unsigned long long>(e.Loops()));
            line += buf;
        }
        out.push_back(line);
        for (const Executor* c : e.Children()) walk(*c, depth + 1);
    };
    walk(root, 0);
    return out;
}

}  // namespace minidb
