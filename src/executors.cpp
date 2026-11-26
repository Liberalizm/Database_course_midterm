#include "executors.h"

#include <algorithm>
#include <climits>

namespace minidb {

namespace {
std::string Join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}
std::string TableLabel(const TableInfo* t, const std::string& alias) {
    return alias == t->name ? t->name : t->name + " " + alias;
}
}  // namespace

// ---------------- SeqScan ----------------

SeqScanExecutor::SeqScanExecutor(TableInfo* table, std::string alias, Schema schema, ExprPtr pred)
    : table_(table), alias_(std::move(alias)), pred_(std::move(pred)) {
    schema_ = std::move(schema);
}

void SeqScanExecutor::InitImpl() { it_.emplace(table_->heap->Begin()); }

bool SeqScanExecutor::NextImpl(Row* out) {
    RID rid;
    std::string buf;
    while (it_->Next(&rid, &buf)) {
        Row r = DeserializeTuple(table_->schema, buf.data(), buf.size());
        if (!pred_ || IsTrue(Evaluate(*pred_, r))) {
            *out = std::move(r);
            cur_ = rid;
            return true;
        }
    }
    return false;
}

std::string SeqScanExecutor::Describe() const {
    std::string s = "Seq Scan on " + TableLabel(table_, alias_);
    if (pred_) s += "  Filter: " + pred_->ToString();
    return s;
}

// ---------------- IndexScan ----------------

IndexScanExecutor::IndexScanExecutor(TableInfo* table, IndexInfo* index, std::string alias, Schema schema,
                                     std::optional<IndexBound> lo, std::optional<IndexBound> hi, ExprPtr pred)
    : table_(table), index_(index), alias_(std::move(alias)), lo_(lo), hi_(hi), pred_(std::move(pred)) {
    schema_ = std::move(schema);
}

void IndexScanExecutor::InitImpl() {
    done_ = false;
    if (lo_) {
        if (!lo_->inclusive && lo_->value == INT64_MAX) {
            done_ = true;
            return;
        }
        it_.emplace(index_->tree->LowerBound(lo_->inclusive ? lo_->value : lo_->value + 1));
    } else {
        it_.emplace(index_->tree->Begin());
    }
}

bool IndexScanExecutor::NextImpl(Row* out) {
    if (done_) return false;
    std::string buf;
    while (it_->Valid()) {
        IndexKey e = it_->Entry();
        if (hi_ && (e.key > hi_->value || (!hi_->inclusive && e.key == hi_->value))) {
            done_ = true;
            return false;
        }
        it_->Next();
        if (!table_->heap->Get(e.rid, &buf)) continue;  // stale entry (should not happen)
        Row r = DeserializeTuple(table_->schema, buf.data(), buf.size());
        if (!pred_ || IsTrue(Evaluate(*pred_, r))) {
            *out = std::move(r);
            cur_ = e.rid;
            return true;
        }
    }
    done_ = true;
    return false;
}

std::string IndexScanExecutor::Describe() const {
    std::string s = "Index Scan using " + index_->name + " on " + TableLabel(table_, alias_);
    std::vector<std::string> conds;
    std::string col = alias_ + "." + index_->column;
    if (lo_ && hi_ && lo_->inclusive && hi_->inclusive && lo_->value == hi_->value) {
        conds.push_back(col + " = " + std::to_string(lo_->value));
    } else {
        if (lo_) conds.push_back(col + (lo_->inclusive ? " >= " : " > ") + std::to_string(lo_->value));
        if (hi_) conds.push_back(col + (hi_->inclusive ? " <= " : " < ") + std::to_string(hi_->value));
    }
    if (!conds.empty()) s += "  Index Cond: " + Join(conds, " AND ");
    if (pred_) s += "  Filter: " + pred_->ToString();
    return s;
}

// ---------------- Filter ----------------

FilterExecutor::FilterExecutor(ExecPtr child, ExprPtr pred, std::string label)
    : child_(std::move(child)), pred_(std::move(pred)), label_(std::move(label)) {
    schema_ = child_->OutSchema();
}

bool FilterExecutor::NextImpl(Row* out) {
    while (child_->Next(out)) {
        if (IsTrue(Evaluate(*pred_, *out))) return true;
    }
    return false;
}

std::string FilterExecutor::Describe() const { return label_ + ": " + pred_->ToString(); }

// ---------------- Projection ----------------

ProjectionExecutor::ProjectionExecutor(ExecPtr child, std::vector<ExprPtr> exprs, std::vector<std::string> names)
    : child_(std::move(child)), exprs_(std::move(exprs)) {
    std::vector<Column> cols;
    for (size_t i = 0; i < exprs_.size(); ++i) {
        Column c;
        c.name = names[i];
        c.type = TypeId::NULL_T;
        if (exprs_[i]->kind == ExprKind::COLUMN && exprs_[i]->col_idx >= 0)
            c.type = child_->OutSchema().At(static_cast<size_t>(exprs_[i]->col_idx)).type;
        cols.push_back(c);
    }
    schema_ = Schema(std::move(cols));
}

bool ProjectionExecutor::NextImpl(Row* out) {
    Row in;
    if (!child_->Next(&in)) return false;
    out->clear();
    out->reserve(exprs_.size());
    for (const auto& e : exprs_) out->push_back(Evaluate(*e, in));
    return true;
}

std::string ProjectionExecutor::Describe() const {
    std::vector<std::string> parts;
    for (size_t i = 0; i < exprs_.size(); ++i) parts.push_back(exprs_[i]->ToString());
    return "Projection: " + Join(parts, ", ");
}

// ---------------- Nested loop join ----------------

NestedLoopJoinExecutor::NestedLoopJoinExecutor(ExecPtr left, ExecPtr right, ExprPtr pred)
    : left_(std::move(left)), right_(std::move(right)), pred_(std::move(pred)) {
    schema_ = Schema::Concat(left_->OutSchema(), right_->OutSchema());
}

void NestedLoopJoinExecutor::InitImpl() {
    left_->Init();
    have_left_ = false;
}

bool NestedLoopJoinExecutor::NextImpl(Row* out) {
    while (true) {
        if (!have_left_) {
            if (!left_->Next(&left_row_)) return false;
            right_->Init();  // rescan inner side
            have_left_ = true;
        }
        Row r;
        while (right_->Next(&r)) {
            Row c = left_row_;
            c.insert(c.end(), r.begin(), r.end());
            if (!pred_ || IsTrue(Evaluate(*pred_, c))) {
                *out = std::move(c);
                return true;
            }
        }
        have_left_ = false;
    }
}

std::string NestedLoopJoinExecutor::Describe() const {
    return "Nested Loop Join" + (pred_ ? "  Join Filter: " + pred_->ToString() : std::string("  (cross product)"));
}

// ---------------- Hash join ----------------

HashJoinExecutor::HashJoinExecutor(ExecPtr probe, ExecPtr build, std::vector<ExprPtr> probe_keys,
                                   std::vector<ExprPtr> build_keys, ExprPtr residual, std::string cond_text)
    : probe_(std::move(probe)),
      build_(std::move(build)),
      probe_keys_(std::move(probe_keys)),
      build_keys_(std::move(build_keys)),
      residual_(std::move(residual)),
      cond_text_(std::move(cond_text)) {
    schema_ = Schema::Concat(probe_->OutSchema(), build_->OutSchema());
}

bool HashJoinExecutor::MakeKey(const std::vector<ExprPtr>& keys, const Row& row, std::string* out) {
    out->clear();
    for (const auto& k : keys) {
        Value v = Evaluate(*k, row);
        if (v.IsNull()) return false;  // NULL never joins
        v.AppendKey(out);
    }
    return true;
}

void HashJoinExecutor::InitImpl() {
    table_.clear();
    build_->Init();
    Row r;
    std::string key;
    while (build_->Next(&r)) {
        if (MakeKey(build_keys_, r, &key)) table_[key].push_back(r);
    }
    probe_->Init();
    matches_ = nullptr;
    match_idx_ = 0;
}

bool HashJoinExecutor::NextImpl(Row* out) {
    std::string key;
    while (true) {
        if (matches_ && match_idx_ < matches_->size()) {
            const Row& b = (*matches_)[match_idx_++];
            Row c = probe_row_;
            c.insert(c.end(), b.begin(), b.end());
            if (!residual_ || IsTrue(Evaluate(*residual_, c))) {
                *out = std::move(c);
                return true;
            }
            continue;
        }
        matches_ = nullptr;
        if (!probe_->Next(&probe_row_)) return false;
        if (!MakeKey(probe_keys_, probe_row_, &key)) continue;
        auto it = table_.find(key);
        if (it != table_.end()) {
            matches_ = &it->second;
            match_idx_ = 0;
        }
    }
}

std::string HashJoinExecutor::Describe() const {
    std::string s = "Hash Join  Hash Cond: " + cond_text_;
    if (residual_) s += "  Join Filter: " + residual_->ToString();
    return s;
}

// ---------------- Aggregate ----------------

AggregateExecutor::AggregateExecutor(ExecPtr child, std::vector<ExprPtr> groups, std::vector<std::string> group_names,
                                     std::vector<AggSpec> aggs)
    : child_(std::move(child)), groups_(std::move(groups)), aggs_(std::move(aggs)) {
    std::vector<Column> cols;
    for (size_t i = 0; i < groups_.size(); ++i) {
        Column c;
        c.name = group_names[i];
        c.type = TypeId::NULL_T;
        cols.push_back(c);
    }
    for (const auto& a : aggs_) {
        Column c;
        c.name = a.name;
        c.type = TypeId::NULL_T;
        cols.push_back(c);
    }
    schema_ = Schema(std::move(cols));
}

namespace {
struct AggState {
    int64_t count = 0;
    Value acc;  // SUM / MIN / MAX
    double dsum = 0;
};
}  // namespace

void AggregateExecutor::InitImpl() {
    results_.clear();
    pos_ = 0;
    std::unordered_map<std::string, size_t> index;
    std::vector<Row> group_vals;
    std::vector<std::vector<AggState>> states;

    child_->Init();
    Row r;
    std::string key;
    while (child_->Next(&r)) {
        Row gv;
        key.clear();
        for (const auto& g : groups_) {
            gv.push_back(Evaluate(*g, r));
            gv.back().AppendKey(&key);
        }
        auto [it, inserted] = index.emplace(key, group_vals.size());
        if (inserted) {
            group_vals.push_back(std::move(gv));
            states.emplace_back(aggs_.size());
        }
        auto& st = states[it->second];
        for (size_t i = 0; i < aggs_.size(); ++i) {
            const AggSpec& a = aggs_[i];
            AggState& s = st[i];
            if (!a.arg) {  // COUNT(*)
                ++s.count;
                continue;
            }
            Value v = Evaluate(*a.arg, r);
            if (v.IsNull()) continue;
            ++s.count;
            switch (a.func) {
                case AggFunc::COUNT: break;
                case AggFunc::SUM:
                    if (!v.IsNumeric()) throw DbError("SUM requires a numeric argument");
                    if (s.acc.IsNull())
                        s.acc = v;
                    else if (s.acc.Type() == TypeId::INT && v.Type() == TypeId::INT)
                        s.acc = Value::Int(s.acc.AsInt() + v.AsInt());
                    else
                        s.acc = Value::Double(s.acc.AsDouble() + v.AsDouble());
                    break;
                case AggFunc::AVG:
                    if (!v.IsNumeric()) throw DbError("AVG requires a numeric argument");
                    s.dsum += v.AsDouble();
                    break;
                case AggFunc::MIN:
                    if (s.acc.IsNull() || v.Compare(s.acc) < 0) s.acc = v;
                    break;
                case AggFunc::MAX:
                    if (s.acc.IsNull() || v.Compare(s.acc) > 0) s.acc = v;
                    break;
            }
        }
    }
    // An aggregate without GROUP BY always returns exactly one row.
    if (groups_.empty() && group_vals.empty()) {
        group_vals.emplace_back();
        states.emplace_back(aggs_.size());
    }
    for (size_t g = 0; g < group_vals.size(); ++g) {
        Row out = group_vals[g];
        for (size_t i = 0; i < aggs_.size(); ++i) {
            const AggState& s = states[g][i];
            switch (aggs_[i].func) {
                case AggFunc::COUNT: out.push_back(Value::Int(s.count)); break;
                case AggFunc::AVG:
                    out.push_back(s.count ? Value::Double(s.dsum / static_cast<double>(s.count)) : Value::Null());
                    break;
                default: out.push_back(s.acc); break;
            }
        }
        results_.push_back(std::move(out));
    }
}

bool AggregateExecutor::NextImpl(Row* out) {
    if (pos_ >= results_.size()) return false;
    *out = results_[pos_++];
    return true;
}

std::string AggregateExecutor::Describe() const {
    std::vector<std::string> g, a;
    for (const auto& e : groups_) g.push_back(e->ToString());
    for (const auto& s : aggs_) a.push_back(s.name);
    std::string d = groups_.empty() ? "Aggregate" : "HashAggregate  Group Key: " + Join(g, ", ");
    if (!a.empty()) d += "  Aggs: " + Join(a, ", ");
    return d;
}

// ---------------- Sort ----------------

SortExecutor::SortExecutor(ExecPtr child, std::vector<std::pair<ExprPtr, bool>> keys)
    : child_(std::move(child)), keys_(std::move(keys)) {
    schema_ = child_->OutSchema();
}

void SortExecutor::InitImpl() {
    rows_.clear();
    pos_ = 0;
    child_->Init();
    std::vector<std::pair<Row, Row>> tmp;  // (sort keys, row)
    Row r;
    while (child_->Next(&r)) {
        Row k;
        for (const auto& [e, desc] : keys_) k.push_back(Evaluate(*e, r));
        tmp.emplace_back(std::move(k), std::move(r));
    }
    std::stable_sort(tmp.begin(), tmp.end(), [this](const auto& a, const auto& b) {
        for (size_t i = 0; i < keys_.size(); ++i) {
            const Value& x = a.first[i];
            const Value& y = b.first[i];
            int c;
            if (x.IsNull() || y.IsNull())
                c = x.IsNull() == y.IsNull() ? 0 : (x.IsNull() ? 1 : -1);  // NULLs sort last
            else
                c = x.Compare(y);
            if (c != 0) return keys_[i].second ? c > 0 : c < 0;
        }
        return false;
    });
    rows_.reserve(tmp.size());
    for (auto& p : tmp) rows_.push_back(std::move(p.second));
}

bool SortExecutor::NextImpl(Row* out) {
    if (pos_ >= rows_.size()) return false;
    *out = rows_[pos_++];
    return true;
}

std::string SortExecutor::Describe() const {
    std::vector<std::string> parts;
    for (const auto& [e, desc] : keys_) parts.push_back(e->ToString() + (desc ? " DESC" : ""));
    return "Sort  Key: " + Join(parts, ", ");
}

// ---------------- Limit ----------------

LimitExecutor::LimitExecutor(ExecPtr child, int64_t limit, int64_t offset)
    : child_(std::move(child)), limit_(limit), offset_(offset) {
    schema_ = child_->OutSchema();
}

void LimitExecutor::InitImpl() {
    child_->Init();
    produced_ = 0;
    Row skip;
    for (int64_t i = 0; i < offset_ && child_->Next(&skip); ++i) {
    }
}

bool LimitExecutor::NextImpl(Row* out) {
    if (limit_ >= 0 && produced_ >= limit_) return false;  // stop pulling early
    if (!child_->Next(out)) return false;
    ++produced_;
    return true;
}

std::string LimitExecutor::Describe() const {
    std::string s = "Limit";
    if (limit_ >= 0) s += " " + std::to_string(limit_);
    if (offset_ > 0) s += " Offset " + std::to_string(offset_);
    return s;
}

// ---------------- Distinct ----------------

DistinctExecutor::DistinctExecutor(ExecPtr child) : child_(std::move(child)) { schema_ = child_->OutSchema(); }

void DistinctExecutor::InitImpl() {
    seen_.clear();
    child_->Init();
}

bool DistinctExecutor::NextImpl(Row* out) {
    std::string key;
    while (child_->Next(out)) {
        key.clear();
        for (const auto& v : *out) v.AppendKey(&key);
        if (seen_.insert(key).second) return true;
    }
    return false;
}

}  // namespace minidb
