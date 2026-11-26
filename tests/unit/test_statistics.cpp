// Unit tests: column statistics (ANALYZE) and the estimates derived from them.
#include "statistics.h"
#include "test_util.h"

namespace minidb {
namespace {

using namespace testing;

// Statistics of a one-column table holding `values`.
ColumnStats StatsOf(const std::vector<Value>& values, TypeId type = TypeId::INT) {
    Schema s({Col("x", type)});
    size_t i = 0;
    auto stats = ComputeStats(s, [&](Row* r) {
        if (i == values.size()) return false;
        *r = {values[i++]};
        return true;
    });
    return stats.at(0);
}

std::vector<Value> Range(int64_t from, int64_t to) {  // [from, to)
    std::vector<Value> v;
    for (int64_t i = from; i < to; ++i) v.push_back(I(i));
    return v;
}

TEST(Statistics, CountsNullsAndDistinctValues) {
    std::vector<Value> v = Range(0, 100);
    v.push_back(I(5));
    v.push_back(N());
    v.push_back(N());
    ColumnStats s = StatsOf(v);
    EXPECT_EQ(s.rows, 103);
    EXPECT_EQ(s.nulls, 2);
    EXPECT_EQ(s.distinct, 100);
    EXPECT_NEAR(s.NullFrac(), 2.0 / 103, 1e-12);
    ASSERT_GE(s.histogram.size(), 2u);
    EXPECT_EQ(s.histogram.front().AsInt(), 0);
    EXPECT_EQ(s.histogram.back().AsInt(), 99);
}

TEST(Statistics, UniformValuesHaveNoMostCommonValues) {
    ColumnStats s = StatsOf(Range(0, 1000));
    EXPECT_TRUE(s.mcv.empty());
    EXPECT_NEAR(*s.EqFrac(I(500)), 0.001, 1e-9);
    EXPECT_EQ(*s.EqFrac(I(5000)), 0.0) << "outside the values seen";
    EXPECT_EQ(*s.EqFrac(I(-1)), 0.0);
}

TEST(Statistics, SkewedValuesBecomeMostCommonValues) {
    std::vector<Value> v;
    for (int i = 0; i < 900; ++i) v.push_back(T("done"));
    for (int i = 0; i < 90; ++i) v.push_back(T("open"));
    for (int i = 0; i < 10; ++i) v.push_back(T("x" + std::to_string(i)));
    ColumnStats s = StatsOf(v, TypeId::TEXT);
    ASSERT_GE(s.mcv.size(), 2u);
    EXPECT_EQ(s.mcv[0].AsText(), "done") << "most frequent first";
    EXPECT_EQ(s.mcv_counts[0], 900);
    EXPECT_NEAR(*s.EqFrac(T("done")), 0.9, 1e-9);
    EXPECT_NEAR(*s.EqFrac(T("open")), 0.09, 1e-9);
    EXPECT_NEAR(*s.EqFrac(T("x3")), 0.001, 1e-9) << "the rest is spread over the other values";
    EXPECT_FALSE(s.EqFrac(I(1))) << "a value the column cannot be compared with";
}

TEST(Statistics, RangesFromTheHistogram) {
    ColumnStats s = StatsOf(Range(0, 10000));
    EXPECT_NEAR(*s.LessFrac(I(2500), false), 0.25, 0.01);
    EXPECT_NEAR(*s.LessFrac(I(9000), true), 0.90, 0.01);
    EXPECT_EQ(*s.LessFrac(I(-5), true), 0.0);
    EXPECT_EQ(*s.LessFrac(I(0), false), 0.0);
    EXPECT_NEAR(*s.LessFrac(I(20000), false), 1.0, 1e-12);
}

TEST(Statistics, HistogramFollowsSkewedDistributions) {
    // 90% of the values are below 100, the rest spread up to 10000.
    std::vector<Value> v;
    for (int i = 0; i < 9000; ++i) v.push_back(I(i % 100));
    for (int i = 0; i < 1000; ++i) v.push_back(I(100 + i * 10));
    ColumnStats s = StatsOf(v);
    EXPECT_NEAR(*s.LessFrac(I(100), false), 0.9, 0.04) << "a min/max model would say about 1%";
    EXPECT_NEAR(*s.LessFrac(I(5000), false), 0.95, 0.04);
}

TEST(Statistics, RangesExcludeNulls) {
    std::vector<Value> v = Range(0, 100);
    for (int i = 0; i < 100; ++i) v.push_back(N());
    ColumnStats s = StatsOf(v);
    EXPECT_NEAR(*s.LessFrac(I(1000), false), 0.5, 1e-9) << "NULL is never < x";
    EXPECT_NEAR(s.AvgEqFrac(), 0.005, 1e-9);
}

TEST(Statistics, DatesAndDoublesInterpolate) {
    std::vector<Value> d;
    for (int i = 0; i < 365; ++i) d.push_back(Value::Date(ParseDate("2024-01-01") + i));
    ColumnStats s = StatsOf(d, TypeId::DATE);
    EXPECT_NEAR(*s.LessFrac(Value::Date(ParseDate("2024-07-01")), false), 182.0 / 365, 0.01);
    EXPECT_NEAR(*s.LessFrac(T("2024-04-01"), false), 91.0 / 365, 0.01) << "text compared as a date";
    std::vector<Value> x;
    for (int i = 0; i < 1000; ++i) x.push_back(D(i / 1000.0));
    EXPECT_NEAR(*StatsOf(x, TypeId::DOUBLE).LessFrac(D(0.3), false), 0.3, 0.01);
}

TEST(Statistics, EmptyAndAllNullColumns) {
    ColumnStats empty = StatsOf({});
    EXPECT_EQ(empty.rows, 0);
    EXPECT_EQ(*empty.EqFrac(I(1)), 0.0);
    ColumnStats nulls = StatsOf({N(), N()});
    EXPECT_EQ(nulls.distinct, 0);
    EXPECT_TRUE(nulls.histogram.empty());
    EXPECT_EQ(*nulls.LessFrac(I(1), true), 0.0);
    EXPECT_EQ(nulls.NullFrac(), 1.0);
    ColumnStats one = StatsOf({I(7)});
    EXPECT_EQ(*one.EqFrac(I(7)), 1.0);
    EXPECT_EQ(*one.LessFrac(I(7), true), 1.0);
    EXPECT_EQ(*one.LessFrac(I(7), false), 0.0);
}

TEST(Statistics, LargeTablesAreSampledButDistinctCountsStayExact) {
    std::vector<Value> v;
    for (int i = 0; i < 100000; ++i) v.push_back(I(i % 40000));
    ColumnStats s = StatsOf(v);
    EXPECT_EQ(s.rows, 100000);
    EXPECT_EQ(s.distinct, 40000);
    EXPECT_NEAR(*s.LessFrac(I(20000), false), 0.6, 0.03) << "0..19999 occur three times, the rest twice";
}

}  // namespace
}  // namespace minidb
