#include "statistics.h"

#include <algorithm>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace minidb {

namespace {

// A position on a line for interpolation inside a histogram bucket.
std::optional<double> Numeric(const Value& v) {
    switch (v.Type()) {
        case TypeId::INT:
        case TypeId::DOUBLE: return v.AsDouble();
        case TypeId::DATE:
        case TypeId::TIMESTAMP: return static_cast<double>(v.AsSeconds());
        default: return std::nullopt;
    }
}

// Compare that reports incomparable values instead of throwing.
std::optional<int> TryCompare(const Value& a, const Value& b) {
    try {
        return a.Compare(b);
    } catch (const DbError&) {
        return std::nullopt;
    }
}

}  // namespace

double ColumnStats::NullFrac() const { return rows > 0 ? static_cast<double>(nulls) / static_cast<double>(rows) : 0.0; }

double ColumnStats::AvgEqFrac() const {
    if (rows <= 0 || distinct <= 0) return 0.0;
    return (1.0 - NullFrac()) / static_cast<double>(distinct);
}

std::optional<double> ColumnStats::EqFrac(const Value& v) const {
    if (rows <= 0) return 0.0;
    if (histogram.empty()) return 0.0;  // only NULLs
    for (size_t i = 0; i < mcv.size(); ++i) {
        auto c = TryCompare(v, mcv[i]);
        if (!c) return std::nullopt;
        if (*c == 0) return static_cast<double>(mcv_counts[i]) / static_cast<double>(rows);
    }
    auto lo = TryCompare(v, histogram.front()), hi = TryCompare(v, histogram.back());
    if (!lo || !hi) return std::nullopt;
    if (*lo < 0 || *hi > 0) return 0.0;  // outside the values seen
    int64_t in_mcv = 0;
    for (int64_t c : mcv_counts) in_mcv += c;
    const int64_t rest_rows = rows - nulls - in_mcv;
    const int64_t rest_distinct = distinct - static_cast<int64_t>(mcv.size());
    if (rest_rows <= 0 || rest_distinct <= 0) return 0.0;
    return static_cast<double>(rest_rows) / static_cast<double>(rest_distinct) / static_cast<double>(rows);
}

std::optional<double> ColumnStats::LessFrac(const Value& value, bool inclusive) const {
    if (rows <= 0 || histogram.empty()) return 0.0;
    // Text compared with a date or timestamp column is read as one, so it can be
    // interpolated like the column's values.
    Value v = value;
    if (v.Type() == TypeId::TEXT && IsTemporal(histogram.front().Type())) {
        try {
            v = v.CastTo(histogram.front().Type());
        } catch (const DbError&) {
            return std::nullopt;
        }
    }
    const auto& h = histogram;
    const size_t k = h.size() - 1;  // buckets
    double below;                   // fraction of the non-NULL values below v
    auto first = TryCompare(v, h.front()), last = TryCompare(v, h.back());
    if (!first || !last) return std::nullopt;
    if (*first < 0 || (*first == 0 && !inclusive)) {
        below = 0.0;
    } else if (*last > 0 || (*last == 0 && inclusive) || k == 0) {
        below = 1.0;
    } else {
        // The bucket [h[i], h[i+1]] that holds v: the last bound <= v.
        size_t i = 0;
        while (i + 1 < k && h[i + 1].Compare(v) <= 0) ++i;
        double within = 0.5;
        auto a = Numeric(h[i]), b = Numeric(h[i + 1]), x = Numeric(v);
        if (a && b && x && *b > *a) within = std::clamp((*x - *a) / (*b - *a), 0.0, 1.0);
        below = (static_cast<double>(i) + within) / static_cast<double>(k);
    }
    double frac = below * (1.0 - NullFrac());
    // Values equal to v: part of "<=", not of "<". The histogram cannot tell
    // them apart, so they are added or removed explicitly.
    if (*first >= 0 && *last <= 0) {
        if (auto eq = EqFrac(v)) frac = inclusive ? frac + *eq * 0.5 : frac - *eq * 0.5;
    }
    return std::clamp(frac, 0.0, 1.0 - NullFrac());
}

std::vector<ColumnStats> ComputeStats(const Schema& schema, const std::function<bool(Row*)>& next_row) {
    const size_t n = schema.Size();
    std::vector<ColumnStats> stats(n);
    std::vector<std::unordered_set<std::string>> distinct(n);
    // A uniform sample of the rows (reservoir sampling), for MCVs and histograms.
    std::vector<Row> sample;
    std::mt19937_64 rng(0x5eed);
    int64_t rows = 0;
    Row row;
    while (next_row(&row)) {
        ++rows;
        for (size_t c = 0; c < n; ++c) {
            if (row[c].IsNull()) {
                ++stats[c].nulls;
                continue;
            }
            std::string key;
            row[c].AppendKey(&key);
            distinct[c].insert(std::move(key));
        }
        if (sample.size() < ColumnStats::kSampleRows) {
            sample.push_back(row);
        } else {
            auto j = static_cast<size_t>(rng() % static_cast<uint64_t>(rows));
            if (j < sample.size()) sample[j] = row;
        }
    }
    const double scale = sample.empty() ? 1.0 : static_cast<double>(rows) / static_cast<double>(sample.size());

    for (size_t c = 0; c < n; ++c) {
        ColumnStats& s = stats[c];
        s.rows = rows;
        s.distinct = static_cast<int64_t>(distinct[c].size());
        distinct[c] = {};
        std::vector<Value> values;
        for (const Row& r : sample)
            if (!r[c].IsNull()) values.push_back(r[c]);
        if (values.empty()) continue;
        std::sort(values.begin(), values.end(), [](const Value& a, const Value& b) { return a.Compare(b) < 0; });

        // MCVs: runs of equal values in the sorted sample, longer than average.
        std::vector<std::pair<int64_t, size_t>> runs;  // (count, index of first)
        for (size_t i = 0; i < values.size();) {
            size_t j = i + 1;
            while (j < values.size() && values[j].Compare(values[i]) == 0) ++j;
            runs.emplace_back(static_cast<int64_t>(j - i), i);
            i = j;
        }
        const double avg = static_cast<double>(values.size()) / static_cast<double>(runs.size());
        std::stable_sort(runs.begin(), runs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [count, first] : runs) {
            if (s.mcv.size() == ColumnStats::kMaxMcv || count < 2 || static_cast<double>(count) <= avg) break;
            s.mcv.push_back(values[first]);
            s.mcv_counts.push_back(std::max<int64_t>(1, static_cast<int64_t>(static_cast<double>(count) * scale)));
        }

        // Equi-depth histogram over all sampled values.
        const size_t k = std::min(ColumnStats::kBuckets, values.size() - 1);
        for (size_t b = 0; b <= k; ++b) {
            size_t pos = k == 0 ? 0 : b * (values.size() - 1) / k;
            s.histogram.push_back(values[pos]);
        }
    }
    return stats;
}

}  // namespace minidb
