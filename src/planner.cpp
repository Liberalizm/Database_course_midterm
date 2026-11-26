#include "planner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

#include "parser.h"

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

// Does `e` have the shape  alias.column <op> <constant>  (either order), with a
// constant an index on a column of type `type` can look up? INT column: any
// comparison with an INT constant. TEXT column (indexed by hash): only equality
// with a TEXT constant. Returns the operator (column on the left) and the constant.
bool MatchKeyPredicate(const Expr& e, const std::string& alias, const std::string& column, TypeId type, BinOp* op,
                       const Value** constant) {
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
    if (c->table != alias || c->column != column) return false;
    if (k->value.Type() != type) return false;
    if (type == TypeId::TEXT && o != BinOp::EQ) return false;
    if (!IsIndexable(type)) return false;
    *op = o;
    *constant = &k->value;
    return true;
}

// What the conditions on one key column allow: an equality (its constant) or
// a range on the column's tree key, and which conjuncts these came from.
struct KeyColumnConds {
    std::optional<size_t> eq_conj;
    const Value* eq_value = nullptr;
    std::optional<IndexBound> lo, hi;
    std::vector<size_t> range_used;
};

void TightenLo(std::optional<IndexBound>& lo, int64_t v, bool incl) {
    if (!lo || v > lo->value || (v == lo->value && !incl)) lo = IndexBound{v, incl};
}
void TightenHi(std::optional<IndexBound>& hi, int64_t v, bool incl) {
    if (!hi || v < hi->value || (v == hi->value && !incl)) hi = IndexBound{v, incl};
}

KeyColumnConds CollectKeyConds(const std::vector<ExprPtr>& conjuncts, const std::string& alias,
                               const std::string& column, TypeId type) {
    KeyColumnConds k;
    for (size_t i = 0; i < conjuncts.size(); ++i) {
        BinOp op;
        const Value* c;
        if (!MatchKeyPredicate(*conjuncts[i], alias, column, type, &op, &c)) continue;
        int64_t v = IndexKeyPart(*c);
        switch (op) {
            case BinOp::EQ:
                if (!k.eq_conj) {
                    k.eq_conj = i;
                    k.eq_value = c;
                }
                break;
            case BinOp::GT: TightenLo(k.lo, v, false); k.range_used.push_back(i); break;
            case BinOp::GE: TightenLo(k.lo, v, true); k.range_used.push_back(i); break;
            case BinOp::LT: TightenHi(k.hi, v, false); k.range_used.push_back(i); break;
            case BinOp::LE: TightenHi(k.hi, v, true); k.range_used.push_back(i); break;
            default: break;
        }
    }
    return k;
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
    std::vector<size_t> best_answered;

    // Option 2..n: an index scan for every usable index. A multi-column index is
    // searched on its leading column, and once that is fixed by an equality, also
    // on the next: a range (or equality) on an INT second column of a two-column
    // index, or equality on all the other columns (whose hash is key2).
    for (IndexInfo* idx : catalog_->IndexesOf(t->name)) {
        auto type_of = [&](size_t i) { return t->schema.At(static_cast<size_t>(idx->col_idxs[i])).type; };
        const KeyColumnConds k0 = CollectKeyConds(conjuncts, src.alias, idx->columns[0], type_of(0));
        std::optional<IndexBound> lo = k0.lo, hi = k0.hi;
        // Statistics: min/max of the column straight from the B+ tree. Assume
        // values are uniformly distributed in [min, max].
        int64_t kmin = 0, kmax = 0;
        // (Not for a hashed index: the min/max of hash values says nothing.)
        bool have_stats = !idx->hashed && idx->tree->MinKey(&kmin) && idx->tree->MaxKey(&kmax) && kmax >= kmin;
        double domain = have_stats ? static_cast<double>(kmax) - static_cast<double>(kmin) + 1.0 : 0.0;

        double matched;
        std::vector<size_t> used;      // conjuncts the scan bounds come from (for estimates)
        std::vector<size_t> answered;  // ... of which the scan alone guarantees (no re-check)
        if (k0.eq_conj) {
            const int64_t eq_val = IndexKeyPart(*k0.eq_value);
            lo = hi = IndexBound{eq_val, true};
            used = {*k0.eq_conj};
            bool full_key = !idx->Composite();  // every key column fixed by an equality
            double narrow = 1.0;                 // selectivity of the conditions on the other columns
            if (idx->OrderedKey2()) {
                KeyColumnConds k1 = CollectKeyConds(conjuncts, src.alias, idx->columns[1], TypeId::INT);
                if (k1.eq_conj) {
                    lo = hi = IndexBound{eq_val, true, true, k1.eq_value->AsInt()};
                    used.push_back(*k1.eq_conj);
                    full_key = true;
                    narrow = 0.05;
                } else if (k1.lo || k1.hi) {
                    if (k1.lo) lo = IndexBound{eq_val, k1.lo->inclusive, true, k1.lo->value};
                    if (k1.hi) hi = IndexBound{eq_val, k1.hi->inclusive, true, k1.hi->value};
                    used.insert(used.end(), k1.range_used.begin(), k1.range_used.end());
                    narrow = (k1.lo && k1.hi) ? 0.1 : 0.3;
                }
            } else if (idx->Key2Hashed()) {
                Row rest;
                std::vector<size_t> rest_used;
                for (size_t i = 1; i < idx->columns.size(); ++i) {
                    KeyColumnConds ki = CollectKeyConds(conjuncts, src.alias, idx->columns[i], type_of(i));
                    if (!ki.eq_conj) break;
                    rest.push_back(*ki.eq_value);
                    rest_used.push_back(*ki.eq_conj);
                }
                if (rest.size() == idx->columns.size() - 1) {
                    lo = hi = IndexBound{eq_val, true, true, IndexKey2(rest.data(), rest.size())};
                    used.insert(used.end(), rest_used.begin(), rest_used.end());
                    full_key = true;
                    narrow = std::pow(0.05, static_cast<double>(rest.size()));
                }
            }
            if (idx->unique && full_key)
                matched = 1.0;
            else if (have_stats)
                matched = (eq_val < kmin || eq_val > kmax) ? 1.0 : std::max(1.0, rows / domain * narrow);
            else
                matched = std::max(1.0, rows * 0.05 * narrow);
            answered.push_back(*k0.eq_conj);
        } else if (lo || hi) {
            used = k0.range_used;
            answered = k0.range_used;
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
        // Only conditions on an INT leading column are answered exactly. A TEXT one
        // is looked up by hash, and key2 can be a hash or stand for a NULL, so
        // those conditions stay in the filter as a re-check.
        if (idx->hashed) answered.clear();
        std::vector<ExprPtr> rest;
        for (size_t i = 0; i < conjuncts.size(); ++i)
            if (std::find(used.begin(), used.end(), i) == used.end()) rest.push_back(conjuncts[i]);

        // Cardenas' formula: expected number of distinct heap pages touched when
        // fetching `matched` random rows out of `pages` pages.
        double pages_fetched = pages * (1.0 - std::pow(1.0 - 1.0 / pages, matched));
        if (pages <= 1.0) pages_fetched = 1.0;
        double cost = idx->tree->Height() + matched / idx->tree->LeafCapacity() + pages_fetched * RANDOM_PAGE_COST +
                      matched * CPU_TUPLE_COST;
        if (cost < best_cost) {
            best_cost = cost;
            best_idx = idx;
            best_lo = lo;
            best_hi = hi;
            best_answered = answered;
            best_rows = matched * SelectivityOf(rest);
        }
    }

    // Conditions the index answers exactly are dropped from the filter.
    std::vector<ExprPtr> residual;
    for (size_t i = 0; i < conjuncts.size(); ++i)
        if (std::find(best_answered.begin(), best_answered.end(), i) == best_answered.end())
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

ExecPtr Planner::PlanView(const std::string& name, const std::vector<std::string>& columns, const SelectStmt& query,
                          Schema* out) {
    if (std::find(view_stack_.begin(), view_stack_.end(), name) != view_stack_.end())
        throw DbError("view \"" + name + "\" refers to itself");
    if (view_stack_.size() >= kMaxViewDepth)
        throw DbError("views are nested more than " + std::to_string(kMaxViewDepth) + " levels deep");
    // The view's query is planned as a query of its own: outer scopes are hidden,
    // so a column it no longer finds (after ALTER TABLE) is an error instead of
    // silently turning into a reference to the query that uses the view.
    struct Restore {
        Planner* p;
        std::vector<Scope*> scopes;
        ~Restore() {
            p->scopes_ = std::move(scopes);
            p->view_stack_.pop_back();
        }
    } restore{this, std::move(scopes_)};
    scopes_.clear();
    view_stack_.push_back(name);

    ExecPtr plan = PlanSelect(query);
    std::vector<Column> cols = plan->OutSchema().Columns();
    if (!columns.empty()) {
        if (columns.size() != cols.size())
            throw DbError("view \"" + name + "\" names " + std::to_string(columns.size()) +
                          " columns but its query returns " + std::to_string(cols.size()));
        for (size_t i = 0; i < cols.size(); ++i) cols[i].name = columns[i];
    }
    for (size_t i = 0; i < cols.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (cols[i].name == cols[j].name)
                throw DbError("view \"" + name + "\" has more than one column named \"" + cols[i].name +
                              "\" (give them different aliases)");
    *out = Schema(std::move(cols));
    return plan;
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
        if (const ViewInfo* v = catalog_->GetView(ref.name)) {
            // A view is expanded into a derived table: its stored query is parsed
            // and planned again here, so it sees the tables as they are now.
            Schema out;
            ExecPtr plan;
            try {
                plan = PlanView(v->name, v->columns, Parser::ParseQuery(v->sql), &out);
            } catch (const DbError& e) {
                throw DbError("view \"" + v->name + "\": " + e.what());
            }
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
    // Outer joins pad rows with NULLs: a LEFT JOIN pads the table it adds, a RIGHT
    // JOIN every table before it, a FULL JOIN both. pad_step[t] is the last join
    // (FROM position) that pads table t, -1 if none. A WHERE condition on a padded
    // table must run after that join, never below it: pushed into the table's scan
    // it would turn the outer join into an inner one. A condition on no table at
    // all (WHERE FALSE) must run after the last RIGHT / FULL JOIN, which would
    // otherwise bring back the rows it removed.
    bool has_outer_join = false;
    int last_keep_left = -1;  // last RIGHT / FULL JOIN
    std::vector<int> pad_step(srcs.size(), -1);
    for (size_t i = 1; i < stmt.from.size(); ++i) {
        JoinType jt = stmt.from[i].join;
        if (jt == JoinType::INNER) continue;
        has_outer_join = true;
        int step = static_cast<int>(i);
        if (jt == JoinType::LEFT || jt == JoinType::FULL) pad_step[i] = step;
        if (jt == JoinType::RIGHT || jt == JoinType::FULL) {
            for (size_t t = 0; t < i; ++t) pad_step[t] = step;
            last_keep_left = step;
        }
    }

    struct Pending {
        ExprPtr e;
        std::set<int> tabs;
        int ready;  // may only run once the join at this FROM position is done
        bool used = false;
    };
    std::vector<Pending> pending;
    std::vector<std::vector<ExprPtr>> per_table(srcs.size());
    for (auto& c : SplitConjuncts(where)) {
        std::set<int> tabs;
        ReferencedTables(*c, srcs, &tabs);
        int ready = tabs.empty() ? last_keep_left : -1;
        for (int t : tabs) ready = std::max(ready, pad_step[static_cast<size_t>(t)]);
        if (tabs.size() == 1 && ready < 0)
            per_table[static_cast<size_t>(*tabs.begin())].push_back(c);  // pushdown
        else
            pending.push_back({c, tabs, ready});
    }

    // ON conditions kept with their join (outer joins, and inner joins before a
    // RIGHT / FULL JOIN). Conditions that only limit which rows of one input can
    // match may filter that input instead:
    //   INNER / LEFT JOIN: conditions on the joined table alone filter its scan;
    //   RIGHT JOIN: conditions on the left input alone filter it (left_filters).
    // A FULL JOIN keeps every row of both inputs, so its ON stays whole.
    // An ON clause may only refer to tables up to and including its own.
    std::vector<std::vector<ExprPtr>> on_conds(srcs.size()), left_filters(srcs.size());
    for (size_t i = 1; i < stmt.from.size(); ++i) {
        if (!stmt.from[i].on) continue;
        JoinType jt = stmt.from[i].join;
        std::vector<Source> visible(srcs.begin(), srcs.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        ExprPtr on = Canonicalize(stmt.from[i].on, visible);
        if (ContainsAggregate(*on)) throw DbError("aggregate functions are not allowed in JOIN conditions");
        for (auto& c : SplitConjuncts(on)) {
            std::set<int> tabs;
            ReferencedTables(*c, srcs, &tabs);
            if ((jt == JoinType::INNER || jt == JoinType::LEFT) && tabs == std::set<int>{static_cast<int>(i)})
                per_table[i].push_back(c);
            else if (jt == JoinType::RIGHT && i == 1 && tabs == std::set<int>{0})
                per_table[0].push_back(c);  // the left input is just that table
            else if (jt == JoinType::RIGHT && !tabs.count(static_cast<int>(i)))
                left_filters[i].push_back(c);
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
    auto add_filter = [&](ExecPtr node, const std::vector<ExprPtr>& conds) {
        if (conds.empty()) return node;
        double est = node->est_rows * SelectivityOf(conds), cost = node->est_cost;
        ExprPtr pred = BindExpr(MakeConjunction(conds), node->OutSchema());
        auto f = std::make_unique<FilterExecutor>(std::move(node), pred);
        f->est_rows = std::max(1.0, est);
        f->est_cost = cost;
        return ExecPtr(std::move(f));
    };
    // Filters `node` (the tables in `joined`, after the join at FROM position
    // `step`) by the pending WHERE conditions that can run there.
    auto apply_ready_filters = [&](ExecPtr node, int step) {
        std::vector<ExprPtr> ready;
        for (auto& p : pending) {
            if (!p.used && IsSubset(p.tabs, joined) && p.ready <= step) {
                ready.push_back(p.e);
                p.used = true;
            }
        }
        return add_filter(std::move(node), ready);
    };

    // Joins `left` (the tables in `joined`) with `right` (table `next`) on `conds`:
    // a hash join if there are equi-join keys, a nested loop join otherwise.
    auto make_join = [&](ExecPtr left, ExecPtr right, int next, const std::vector<ExprPtr>& conds,
                         JoinType type) -> ExecPtr {
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
            // Hash join; the smaller input becomes the build (hashed) side. Either
            // side of an outer join can be the preserved one: the executor's join
            // type is relative to (probe, build), so it flips when `left` is built.
            out_rows = l_rows * r_rows / std::max(l_rows, r_rows) * SelectivityOf(residual);
            cost = l_cost + r_cost + (l_rows + r_rows) * HASH_TUPLE_COST;
            std::string text;
            for (size_t i = 0; i < cond_text.size(); ++i) text += (i ? " AND " : "") + cond_text[i];
            bool build_left = l_rows < r_rows;
            JoinType hash_type = type;
            if (build_left && type == JoinType::LEFT) hash_type = JoinType::RIGHT;
            if (build_left && type == JoinType::RIGHT) hash_type = JoinType::LEFT;
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
                                                      hash_type);
        } else {
            out_rows = l_rows * r_rows * SelectivityOf(residual);
            cost = l_cost + l_rows * r_cost + l_rows * r_rows * CPU_TUPLE_COST;
            Schema combined = Schema::Concat(left->OutSchema(), right->OutSchema());
            ExprPtr pred = residual.empty() ? nullptr : BindExpr(MakeConjunction(residual), combined);
            join = std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right), pred, type);
        }
        // Every row of a preserved side survives.
        if (type == JoinType::LEFT || type == JoinType::FULL) out_rows = std::max(out_rows, l_rows);
        if (type == JoinType::RIGHT || type == JoinType::FULL) out_rows = std::max(out_rows, r_rows);
        join->est_rows = std::max(1.0, out_rows);
        join->est_cost = cost;
        return join;
    };

    // WHERE conditions that can become conditions of the inner join adding `next`.
    // Never those on a table an outer join pads at or after `next`: they must
    // filter after that join (apply_ready_filters takes care of them).
    auto take_applicable = [&](int next) {
        std::set<int> with = joined;
        with.insert(next);
        std::vector<ExprPtr> applicable;
        for (auto& p : pending) {
            if (p.used || !IsSubset(p.tabs, with) || p.ready >= next) continue;
            applicable.push_back(p.e);
            p.used = true;
        }
        return applicable;
    };

    if (srcs.empty()) {
        root = std::make_unique<ValuesExecutor>();
        root = apply_ready_filters(std::move(root), 0);
    } else if (has_outer_join) {
        // Outer joins are not freely reorderable: join in the order written.
        root = std::move(base[0]);
        joined.insert(0);
        root = apply_ready_filters(std::move(root), 0);
        for (size_t i = 1; i < srcs.size(); ++i) {
            int next = static_cast<int>(i);
            JoinType jt = stmt.from[i].join;
            std::vector<ExprPtr> conds = on_conds[i];
            if (jt == JoinType::INNER) {
                auto more = take_applicable(next);
                conds.insert(conds.end(), more.begin(), more.end());
            }
            root = add_filter(std::move(root), left_filters[i]);
            root = make_join(std::move(root), std::move(base[i]), next, conds, jt);
            joined.insert(next);
            root = apply_ready_filters(std::move(root), next);
        }
    } else {
        int start = 0;
        for (size_t i = 1; i < srcs.size(); ++i)
            if (base[i]->est_rows < base[static_cast<size_t>(start)]->est_rows) start = static_cast<int>(i);
        root = std::move(base[static_cast<size_t>(start)]);
        joined.insert(start);
        root = apply_ready_filters(std::move(root), 0);

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
            root = make_join(std::move(root), std::move(base[static_cast<size_t>(next)]), next, applicable,
                             JoinType::INNER);
            joined.insert(next);
            root = apply_ready_filters(std::move(root), next);
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
