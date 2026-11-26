// executors.h - physical operators in the Volcano (iterator) model.
//
// Every operator implements Init() / Next(row). A query plan is a tree of
// operators; the root pulls rows from its children one at a time.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "catalog.h"
#include "expression.h"

namespace minidb {

class Executor {
public:
    virtual ~Executor() = default;

    void Init() {
        ++loops_;
        InitImpl();
    }
    bool Next(Row* out) {
        bool ok = NextImpl(out);
        if (ok) ++actual_rows_;
        return ok;
    }

    const Schema& OutSchema() const { return schema_; }
    virtual std::string Describe() const = 0;
    virtual std::vector<const Executor*> Children() const { return {}; }

    // Planner estimates and runtime counters (shown by EXPLAIN / EXPLAIN ANALYZE).
    double est_rows = 0;
    double est_cost = 0;
    uint64_t ActualRows() const { return actual_rows_; }
    uint64_t Loops() const { return loops_; }

protected:
    virtual void InitImpl() = 0;
    virtual bool NextImpl(Row* out) = 0;
    Schema schema_;

private:
    uint64_t actual_rows_ = 0;
    uint64_t loops_ = 0;
};

using ExecPtr = std::unique_ptr<Executor>;

// Base for operators that read a base table; exposes the RID of the last row
// (needed by DELETE / UPDATE).
class ScanExecutor : public Executor {
public:
    virtual RID CurrentRid() const = 0;
};

class SeqScanExecutor : public ScanExecutor {
public:
    SeqScanExecutor(TableInfo* table, std::string alias, Schema schema, ExprPtr pred);
    RID CurrentRid() const override { return cur_; }
    std::string Describe() const override;

protected:
    void InitImpl() override;
    bool NextImpl(Row* out) override;

private:
    TableInfo* table_;
    std::string alias_;
    ExprPtr pred_;
    std::optional<TableHeap::Iterator> it_;
    RID cur_;
};

struct IndexBound {
    int64_t value;
    bool inclusive;
};

class IndexScanExecutor : public ScanExecutor {
public:
    IndexScanExecutor(TableInfo* table, IndexInfo* index, std::string alias, Schema schema,
                      std::optional<IndexBound> lo, std::optional<IndexBound> hi, ExprPtr pred);
    RID CurrentRid() const override { return cur_; }
    std::string Describe() const override;

protected:
    void InitImpl() override;
    bool NextImpl(Row* out) override;

private:
    TableInfo* table_;
    IndexInfo* index_;
    std::string alias_;
    std::optional<IndexBound> lo_, hi_;
    ExprPtr pred_;
    std::optional<BPlusTree::Iterator> it_;
    bool done_ = false;
    RID cur_;
};

class ValuesExecutor : public Executor {  // a single empty row: SELECT without FROM
public:
    ValuesExecutor() { est_rows = 1; }
    std::string Describe() const override { return "Result"; }

protected:
    void InitImpl() override { done_ = false; }
    bool NextImpl(Row* out) override {
        if (done_) return false;
        done_ = true;
        out->clear();
        return true;
    }

private:
    bool done_ = false;
};

class FilterExecutor : public Executor {
public:
    FilterExecutor(ExecPtr child, ExprPtr pred, std::string label = "Filter");
    std::string Describe() const override;
    std::vector<const Executor*> Children() const override { return {child_.get()}; }

protected:
    void InitImpl() override { child_->Init(); }
    bool NextImpl(Row* out) override;

private:
    ExecPtr child_;
    ExprPtr pred_;
    std::string label_;
};

class ProjectionExecutor : public Executor {
public:
    ProjectionExecutor(ExecPtr child, std::vector<ExprPtr> exprs, std::vector<std::string> names);
    std::string Describe() const override;
    std::vector<const Executor*> Children() const override { return {child_.get()}; }

protected:
    void InitImpl() override { child_->Init(); }
    bool NextImpl(Row* out) override;

private:
    ExecPtr child_;
    std::vector<ExprPtr> exprs_;
};

// For every outer row, re-scan the inner child completely.
class NestedLoopJoinExecutor : public Executor {
public:
    // left_outer: a left row without any matching right row is still emitted once,
    // padded with NULLs (LEFT JOIN).
    NestedLoopJoinExecutor(ExecPtr left, ExecPtr right, ExprPtr pred, bool left_outer = false);
    std::string Describe() const override;
    std::vector<const Executor*> Children() const override { return {left_.get(), right_.get()}; }

protected:
    void InitImpl() override;
    bool NextImpl(Row* out) override;

private:
    ExecPtr left_, right_;
    ExprPtr pred_;
    Row left_row_;
    bool have_left_ = false;
    bool left_outer_;
    bool matched_ = false;  // current left row produced at least one output row
};

// Classic in-memory hash join: build a hash table on the `build` input, then
// stream the `probe` input through it. Output = probe columns ++ build columns.
// With left_outer the probe side is the preserved (left) side of a LEFT JOIN.
class HashJoinExecutor : public Executor {
public:
    HashJoinExecutor(ExecPtr probe, ExecPtr build, std::vector<ExprPtr> probe_keys, std::vector<ExprPtr> build_keys,
                     ExprPtr residual, std::string cond_text, bool left_outer = false);
    std::string Describe() const override;
    std::vector<const Executor*> Children() const override { return {probe_.get(), build_.get()}; }

protected:
    void InitImpl() override;
    bool NextImpl(Row* out) override;

private:
    static bool MakeKey(const std::vector<ExprPtr>& keys, const Row& row, std::string* out);
    ExecPtr probe_, build_;
    std::vector<ExprPtr> probe_keys_, build_keys_;
    ExprPtr residual_;
    std::string cond_text_;
    std::unordered_map<std::string, std::vector<Row>> table_;
    const std::vector<Row>* matches_ = nullptr;
    size_t match_idx_ = 0;
    Row probe_row_;
    bool have_probe_ = false;
    bool left_outer_;
    bool matched_ = false;
};

struct AggSpec {
    AggFunc func;
    ExprPtr arg;  // bound; null for COUNT(*)
    std::string name;
    bool distinct = false;  // COUNT(DISTINCT x) etc.: duplicate values are ignored
};

// Hash aggregation (GROUP BY + aggregate functions).
class AggregateExecutor : public Executor {
public:
    AggregateExecutor(ExecPtr child, std::vector<ExprPtr> groups, std::vector<std::string> group_names,
                      std::vector<AggSpec> aggs);
    std::string Describe() const override;
    std::vector<const Executor*> Children() const override { return {child_.get()}; }

protected:
    void InitImpl() override;
    bool NextImpl(Row* out) override;

private:
    ExecPtr child_;
    std::vector<ExprPtr> groups_;
    std::vector<AggSpec> aggs_;
    std::vector<Row> results_;
    size_t pos_ = 0;
};

class SortExecutor : public Executor {
public:
    SortExecutor(ExecPtr child, std::vector<std::pair<ExprPtr, bool>> keys);  // (expr, descending)
    std::string Describe() const override;
    std::vector<const Executor*> Children() const override { return {child_.get()}; }

protected:
    void InitImpl() override;
    bool NextImpl(Row* out) override;

private:
    ExecPtr child_;
    std::vector<std::pair<ExprPtr, bool>> keys_;
    std::vector<Row> rows_;
    size_t pos_ = 0;
};

class LimitExecutor : public Executor {
public:
    LimitExecutor(ExecPtr child, int64_t limit, int64_t offset);
    std::string Describe() const override;
    std::vector<const Executor*> Children() const override { return {child_.get()}; }

protected:
    void InitImpl() override;
    bool NextImpl(Row* out) override;

private:
    ExecPtr child_;
    int64_t limit_, offset_;
    int64_t produced_ = 0;
};

class DistinctExecutor : public Executor {
public:
    explicit DistinctExecutor(ExecPtr child);
    std::string Describe() const override { return "HashDistinct"; }
    std::vector<const Executor*> Children() const override { return {child_.get()}; }

protected:
    void InitImpl() override;
    bool NextImpl(Row* out) override;

private:
    ExecPtr child_;
    std::unordered_set<std::string> seen_;
};

// UNION / INTERSECT / EXCEPT [ALL] of two inputs with the same number of
// columns. Whole rows are compared, and two NULLs count as equal (SQL set
// semantics, unlike "="). Without ALL the result has no duplicates; with ALL,
// INTERSECT keeps min(m, n) and EXCEPT max(m - n, 0) copies of a row that
// occurs m times on the left and n times on the right.
class SetOpExecutor : public Executor {
public:
    SetOpExecutor(ExecPtr left, ExecPtr right, SetOp op, bool all);
    std::string Describe() const override;
    std::vector<const Executor*> Children() const override { return {left_.get(), right_.get()}; }

protected:
    void InitImpl() override;
    bool NextImpl(Row* out) override;

private:
    static std::string KeyOf(const Row& r);
    ExecPtr left_, right_;
    SetOp op_;
    bool all_;
    bool left_done_ = false;                                // UNION: now streaming the right input
    std::unordered_map<std::string, int64_t> right_counts_;  // INTERSECT / EXCEPT: right rows
    std::unordered_set<std::string> emitted_;                // without ALL: rows already returned
};

}  // namespace minidb
