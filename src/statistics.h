// statistics.h - per-column statistics gathered by ANALYZE, and the estimates
// the planner derives from them.
//
// For every column ANALYZE records
//   * the number of NULLs and of distinct non-NULL values,
//   * the most common values (MCVs) with their exact counts: values seen more
//     often than the average value, at most kMaxMcv of them, and
//   * an equi-depth histogram: k + 1 sorted bounds such that each of the k
//     buckets holds about the same number of the column's non-NULL values.
// Equality with an MCV is estimated from its count; with another value from
// the rows the MCVs leave, spread over the remaining distinct values. A range
// is estimated from the histogram, interpolating linearly inside a bucket for
// numbers, dates and timestamps. Statistics describe the table when it was
// analyzed; the planner applies the fractions to the current row count.
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "value.h"

namespace minidb {

struct ColumnStats {
    static constexpr size_t kMaxMcv = 10;
    static constexpr size_t kBuckets = 32;
    // Above this many rows, MCVs and the histogram come from a uniform sample
    // (distinct counts are always exact).
    static constexpr size_t kSampleRows = 30000;

    int64_t rows = 0;      // rows in the table when it was analyzed
    int64_t nulls = 0;
    int64_t distinct = 0;  // distinct non-NULL values
    std::vector<Value> mcv;           // most common values, most frequent first
    std::vector<int64_t> mcv_counts;  // their counts (scaled up when sampled)
    std::vector<Value> histogram;     // bucket bounds, sorted; empty if all NULL

    double NullFrac() const;
    // Fraction of the table's rows with column = v (v not NULL). nullopt if v
    // cannot be compared with the column's values.
    std::optional<double> EqFrac(const Value& v) const;
    // Fraction of rows with column < v (or <= v).
    std::optional<double> LessFrac(const Value& v, bool inclusive) const;
    // Fraction of rows that equal some given value, for an unknown value.
    double AvgEqFrac() const;
};

// Statistics of every column of a table. `next_row` yields the table's rows
// one by one and returns false at the end.
std::vector<ColumnStats> ComputeStats(const Schema& schema, const std::function<bool(Row*)>& next_row);

}  // namespace minidb
