// Unit tests: physical operators. Most are fed by an in-memory VectorExecutor,
// so each operator is tested on its own, without tables or the planner.
#include <algorithm>
#include <climits>
#include <random>

#include "catalog.h"
#include "executors.h"
#include "test_util.h"

namespace minidb {
namespace {

using namespace testing;

ExecPtr Vec(Schema s, std::vector<Row> rows) { return std::make_unique<VectorExecutor>(std::move(s), std::move(rows)); }
ExprPtr C(Value v) { return Expr::Const(std::move(v)); }
ExprPtr Bin(BinOp op, ExprPtr l, ExprPtr r) { return Expr::Binary(op, std::move(l), std::move(r)); }
ExprPtr ColRef(const Schema& s, const std::string& table, const std::string& name) {
    return BindExpr(Expr::Col(table, name), s);
}

Schema People() { return Schema({Col("id", TypeId::INT, "p"), Col("name", TypeId::TEXT, "p"), Col("dept", TypeId::INT, "p")}); }
std::vector<Row> PeopleRows() {
    return {{I(1), T("ann"), I(10)}, {I(2), T("bob"), I(20)}, {I(3), T("cid"), I(10)}, {I(4), T("dan"), N()}};
}
Schema Depts() { return Schema({Col("id", TypeId::INT, "d"), Col("title", TypeId::TEXT, "d")}); }
std::vector<Row> DeptRows() { return {{I(10), T("eng")}, {I(20), T("ops")}, {I(30), T("hr")}}; }

TEST(Executors, VectorSourceCanBeReinitialized) {
    VectorExecutor v(People(), PeopleRows());
    EXPECT_EQ(Drain(v).size(), 4u);
    EXPECT_EQ(Drain(v).size(), 4u);
    EXPECT_EQ(v.Loops(), 2u);
    EXPECT_EQ(v.ActualRows(), 8u);
}

TEST(Executors, Values) {
    ValuesExecutor v;
    auto rows = Drain(v);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].empty());
}

TEST(Executors, Filter) {
    Schema s = People();
    FilterExecutor f(Vec(s, PeopleRows()), Bin(BinOp::EQ, ColRef(s, "", "dept"), C(I(10))));
    auto rows = Drain(f);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][1].AsText(), "ann");
    EXPECT_EQ(rows[1][1].AsText(), "cid");
}

TEST(Executors, FilterDropsUnknown) {
    Schema s = People();
    // dept <> 10: the NULL dept row yields UNKNOWN and must be dropped
    FilterExecutor f(Vec(s, PeopleRows()), Bin(BinOp::NE, ColRef(s, "", "dept"), C(I(10))));
    EXPECT_EQ(Drain(f).size(), 1u);
}

TEST(Executors, Projection) {
    Schema s = People();
    ProjectionExecutor p(Vec(s, PeopleRows()),
                         {ColRef(s, "", "name"), Bin(BinOp::MUL, ColRef(s, "", "id"), C(I(100)))}, {"name", "x"});
    EXPECT_EQ(p.OutSchema().At(0).name, "name");
    EXPECT_EQ(p.OutSchema().At(0).type, TypeId::TEXT);
    auto rows = Drain(p);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[2][0].AsText(), "cid");
    EXPECT_EQ(rows[2][1].AsInt(), 300);
}

TEST(Executors, NestedLoopJoinCrossProductAndRescan) {
    auto right = Vec(Depts(), DeptRows());
    auto* right_raw = right.get();
    NestedLoopJoinExecutor j(Vec(People(), PeopleRows()), std::move(right), nullptr);
    EXPECT_EQ(Drain(j).size(), 12u);
    EXPECT_EQ(right_raw->Loops(), 4u) << "inner side is rescanned once per outer row";
    EXPECT_EQ(j.OutSchema().Size(), 5u);
}

TEST(Executors, NestedLoopJoinWithPredicate) {
    Schema both = Schema::Concat(People(), Depts());
    NestedLoopJoinExecutor j(Vec(People(), PeopleRows()), Vec(Depts(), DeptRows()),
                             Bin(BinOp::EQ, ColRef(both, "p", "dept"), ColRef(both, "d", "id")));
    auto rows = Drain(j);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[1][1].AsText(), "bob");
    EXPECT_EQ(rows[1][4].AsText(), "ops");
}

TEST(Executors, HashJoinMatchesAndNullKeys) {
    HashJoinExecutor j(Vec(People(), PeopleRows()), Vec(Depts(), DeptRows()),
                       {ColRef(People(), "", "dept")}, {ColRef(Depts(), "", "id")}, nullptr, "p.dept = d.id");
    auto rows = Drain(j);
    ASSERT_EQ(rows.size(), 3u) << "dan (NULL dept) must not join";
    for (const auto& r : rows) EXPECT_EQ(r[2].AsInt(), r[3].AsInt());
}

TEST(Executors, HashJoinDuplicateKeysAndResidual) {
    Schema l({Col("k", TypeId::INT, "l"), Col("v", TypeId::INT, "l")});
    Schema r({Col("k", TypeId::INT, "r"), Col("w", TypeId::INT, "r")});
    std::vector<Row> lrows = {{I(1), I(1)}, {I(1), I(2)}, {I(2), I(3)}};
    std::vector<Row> rrows = {{I(1), I(5)}, {I(1), I(0)}, {I(3), I(9)}};
    Schema both = Schema::Concat(l, r);
    HashJoinExecutor all(Vec(l, lrows), Vec(r, rrows), {ColRef(l, "", "k")}, {ColRef(r, "", "k")}, nullptr, "");
    EXPECT_EQ(Drain(all).size(), 4u);  // 2 x 2 for key 1
    HashJoinExecutor res(Vec(l, lrows), Vec(r, rrows), {ColRef(l, "", "k")}, {ColRef(r, "", "k")},
                         Bin(BinOp::LT, ColRef(both, "l", "v"), ColRef(both, "r", "w")), "");
    EXPECT_EQ(Drain(res).size(), 2u);  // v < w: (1,5), (2,5)
}

TEST(Executors, HashJoinAgreesWithNestedLoopOnRandomData) {
    std::mt19937 rng(123);
    Schema l({Col("a", TypeId::INT, "l")});
    Schema r({Col("b", TypeId::INT, "r")});
    std::vector<Row> lrows, rrows;
    for (int i = 0; i < 300; ++i) lrows.push_back({rng() % 10 == 0 ? N() : I(rng() % 50)});
    for (int i = 0; i < 200; ++i) rrows.push_back({I(rng() % 50)});
    Schema both = Schema::Concat(l, r);
    HashJoinExecutor hj(Vec(l, lrows), Vec(r, rrows), {ColRef(l, "", "a")}, {ColRef(r, "", "b")}, nullptr, "");
    NestedLoopJoinExecutor nl(Vec(l, lrows), Vec(r, rrows), Bin(BinOp::EQ, ColRef(both, "", "a"), ColRef(both, "", "b")));
    EXPECT_EQ(Drain(hj).size(), Drain(nl).size());
}

TEST(Executors, HashJoinIntAndDoubleKeysMatch) {
    Schema l({Col("a", TypeId::INT, "l")});
    Schema r({Col("b", TypeId::DOUBLE, "r")});
    HashJoinExecutor j(Vec(l, {{I(2)}, {I(3)}}), Vec(r, {{D(2.0)}, {D(3.5)}}), {ColRef(l, "", "a")},
                       {ColRef(r, "", "b")}, nullptr, "");
    EXPECT_EQ(Drain(j).size(), 1u);
}

// ---- LEFT JOIN (left_outer) ----

// Rows rendered as text and sorted, so join outputs can be compared as multisets.
std::vector<std::string> Canon(std::vector<Row> rows) {
    std::vector<std::string> out;
    for (const auto& r : rows) {
        std::string s;
        for (const auto& v : r) s += (v.IsNull() ? "NULL" : v.ToString()) + "|";
        out.push_back(s);
    }
    std::sort(out.begin(), out.end());
    return out;
}

TEST(Executors, NestedLoopLeftJoinPadsUnmatchedRowsWithNulls) {
    Schema both = Schema::Concat(People(), Depts());
    NestedLoopJoinExecutor j(Vec(People(), PeopleRows()), Vec(Depts(), DeptRows()),
                             Bin(BinOp::EQ, ColRef(both, "p", "dept"), ColRef(both, "d", "id")), true);
    auto rows = Drain(j);
    ASSERT_EQ(rows.size(), 4u) << "every person exactly once (one dept each at most)";
    EXPECT_EQ(rows[3][1].AsText(), "dan");
    EXPECT_TRUE(rows[3][3].IsNull()) << "NULL dept never matches but the row is kept";
    EXPECT_TRUE(rows[3][4].IsNull());
    EXPECT_EQ(rows[3].size(), 5u);
    EXPECT_NE(j.Describe().find("Left Join"), std::string::npos);
}

TEST(Executors, NestedLoopLeftJoinWithEmptyRightSide) {
    NestedLoopJoinExecutor j(Vec(People(), PeopleRows()), Vec(Depts(), {}), nullptr, true);
    auto rows = Drain(j);
    ASSERT_EQ(rows.size(), 4u);
    for (const auto& r : rows) EXPECT_TRUE(r[3].IsNull() && r[4].IsNull());
    NestedLoopJoinExecutor inner(Vec(People(), PeopleRows()), Vec(Depts(), {}), nullptr);
    EXPECT_TRUE(Drain(inner).empty());
}

TEST(Executors, HashLeftJoinDuplicatesNullKeysAndResidual) {
    Schema l({Col("k", TypeId::INT, "l"), Col("v", TypeId::INT, "l")});
    Schema r({Col("k", TypeId::INT, "r"), Col("w", TypeId::INT, "r")});
    std::vector<Row> lrows = {{I(1), I(1)}, {I(1), I(9)}, {I(2), I(3)}, {N(), I(4)}};
    std::vector<Row> rrows = {{I(1), I(5)}, {I(1), I(0)}, {I(3), I(9)}};
    Schema both = Schema::Concat(l, r);
    // l.k = r.k AND l.v < r.w: (1,1) matches (1,5); (1,9) matches nothing -> padded;
    // (2,3) has no key partner -> padded; (NULL,4) never joins -> padded.
    HashJoinExecutor j(Vec(l, lrows), Vec(r, rrows), {ColRef(l, "", "k")}, {ColRef(r, "", "k")},
                       Bin(BinOp::LT, ColRef(both, "l", "v"), ColRef(both, "r", "w")), "", true);
    auto rows = Canon(Drain(j));
    std::vector<std::string> expected = {"1|1|1|5|", "1|9|NULL|NULL|", "2|3|NULL|NULL|", "NULL|4|NULL|NULL|"};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(rows, expected);
    EXPECT_EQ(Canon(Drain(j)), expected) << "re-initialization starts over";
}

TEST(Executors, HashLeftJoinAgreesWithNestedLoopLeftJoinOnRandomData) {
    for (uint32_t seed = 1; seed <= 5; ++seed) {
        std::mt19937 rng(seed);
        Schema l({Col("a", TypeId::INT, "l"), Col("x", TypeId::INT, "l")});
        Schema r({Col("b", TypeId::INT, "r"), Col("y", TypeId::INT, "r")});
        std::vector<Row> lrows, rrows;
        for (int i = 0; i < 200; ++i) lrows.push_back({rng() % 10 == 0 ? N() : I(rng() % 40), I(rng() % 5)});
        for (int i = 0; i < 150; ++i) rrows.push_back({I(rng() % 60), I(rng() % 5)});
        Schema both = Schema::Concat(l, r);
        ExprPtr residual = Bin(BinOp::LE, ColRef(both, "", "x"), ColRef(both, "", "y"));
        HashJoinExecutor hj(Vec(l, lrows), Vec(r, rrows), {ColRef(l, "", "a")}, {ColRef(r, "", "b")}, residual, "",
                            true);
        NestedLoopJoinExecutor nl(Vec(l, lrows), Vec(r, rrows),
                                  Bin(BinOp::AND, Bin(BinOp::EQ, ColRef(both, "", "a"), ColRef(both, "", "b")), residual),
                                  true);
        auto h = Canon(Drain(hj));
        EXPECT_EQ(h, Canon(Drain(nl))) << "seed " << seed;
        EXPECT_GE(h.size(), lrows.size()) << "a LEFT JOIN never loses left rows";
    }
}

TEST(Executors, AggregateWithGroups) {
    Schema s({Col("g", TypeId::TEXT), Col("x", TypeId::INT)});
    std::vector<Row> rows = {{T("a"), I(1)}, {T("b"), I(10)}, {T("a"), I(3)}, {T("a"), N()}, {T("b"), I(20)}};
    ExprPtr x = ColRef(s, "", "x");
    AggregateExecutor agg(Vec(s, rows), {ColRef(s, "", "g")}, {"g"},
                          {{AggFunc::COUNT, nullptr, "count(*)"},
                           {AggFunc::COUNT, x, "count(x)"},
                           {AggFunc::SUM, x, "sum(x)"},
                           {AggFunc::AVG, x, "avg(x)"},
                           {AggFunc::MIN, x, "min(x)"},
                           {AggFunc::MAX, x, "max(x)"}});
    auto out = Drain(agg);
    ASSERT_EQ(out.size(), 2u);
    const Row& a = out[0][0].AsText() == "a" ? out[0] : out[1];
    EXPECT_EQ(a[1].AsInt(), 3);  // count(*) includes the NULL row
    EXPECT_EQ(a[2].AsInt(), 2);  // count(x) does not
    EXPECT_EQ(a[3].AsInt(), 4);
    EXPECT_EQ(a[3].Type(), TypeId::INT) << "SUM of INTs stays INT";
    EXPECT_DOUBLE_EQ(a[4].AsDouble(), 2.0);
    EXPECT_EQ(a[5].AsInt(), 1);
    EXPECT_EQ(a[6].AsInt(), 3);
    EXPECT_EQ(agg.OutSchema().At(4).name, "avg(x)");
}

TEST(Executors, AggregateOverEmptyInput) {
    Schema s({Col("x", TypeId::INT)});
    ExprPtr x = ColRef(s, "", "x");
    AggregateExecutor no_groups(Vec(s, {}), {}, {}, {{AggFunc::COUNT, nullptr, "c"}, {AggFunc::SUM, x, "s"}});
    auto out = Drain(no_groups);
    ASSERT_EQ(out.size(), 1u) << "aggregate without GROUP BY always yields one row";
    EXPECT_EQ(out[0][0].AsInt(), 0);
    EXPECT_TRUE(out[0][1].IsNull());
    AggregateExecutor grouped(Vec(s, {}), {x}, {"x"}, {{AggFunc::COUNT, nullptr, "c"}});
    EXPECT_TRUE(Drain(grouped).empty());
}

TEST(Executors, AggregateSumPromotesAndRejectsText) {
    Schema s({Col("x", TypeId::NULL_T)});
    ExprPtr x = ColRef(s, "", "x");
    AggregateExecutor mixed(Vec(s, {{I(1)}, {D(0.5)}}), {}, {}, {{AggFunc::SUM, x, "s"}});
    auto out = Drain(mixed);
    EXPECT_EQ(out[0][0].Type(), TypeId::DOUBLE);
    EXPECT_DOUBLE_EQ(out[0][0].AsDouble(), 1.5);
    AggregateExecutor text(Vec(s, {{T("a")}}), {}, {}, {{AggFunc::SUM, x, "s"}});
    EXPECT_THROW(Drain(text), DbError);
}

TEST(Executors, AggregateIntegerSumOverflowIsAnError) {
    Schema s({Col("x", TypeId::INT)});
    ExprPtr x = ColRef(s, "", "x");
    AggregateExecutor at_limit(Vec(s, {{I(INT64_MAX - 1)}, {I(1)}}), {}, {}, {{AggFunc::SUM, x, "s"}});
    EXPECT_EQ(Drain(at_limit)[0][0].AsInt(), INT64_MAX);
    AggregateExecutor over(Vec(s, {{I(INT64_MAX)}, {I(1)}}), {}, {}, {{AggFunc::SUM, x, "s"}});
    EXPECT_THROW(Drain(over), DbError);
    AggregateExecutor avg(Vec(s, {{I(INT64_MAX)}, {I(INT64_MAX)}}), {}, {}, {{AggFunc::AVG, x, "a"}});
    EXPECT_DOUBLE_EQ(Drain(avg)[0][0].AsDouble(), static_cast<double>(INT64_MAX)) << "AVG sums in double";
}

TEST(Executors, DistinctAggregatesIgnoreDuplicatesPerGroup) {
    Schema s({Col("g", TypeId::INT), Col("x", TypeId::NULL_T)});
    ExprPtr g = ColRef(s, "", "g"), x = ColRef(s, "", "x");
    std::vector<Row> rows = {{I(1), I(5)}, {I(1), I(5)}, {I(1), D(5.0)}, {I(1), I(7)}, {I(1), N()},
                             {I(2), I(5)}, {I(2), I(5)}};
    AggSpec cd{AggFunc::COUNT, x, "cd", true};
    AggSpec sd{AggFunc::SUM, x, "sd", true};
    AggSpec c{AggFunc::COUNT, x, "c", false};
    AggSpec ad{AggFunc::AVG, x, "ad", true};
    AggregateExecutor agg(Vec(s, rows), {g}, {"g"}, {cd, sd, c, ad});
    auto out = Drain(agg);
    ASSERT_EQ(out.size(), 2u);
    const Row& g1 = out[0][0].AsInt() == 1 ? out[0] : out[1];
    const Row& g2 = out[0][0].AsInt() == 1 ? out[1] : out[0];
    EXPECT_EQ(g1[1].AsInt(), 2) << "5, 5.0 and 5 are one value; NULL is ignored";
    EXPECT_EQ(g1[2].AsInt(), 12);
    EXPECT_EQ(g1[3].AsInt(), 4);
    EXPECT_DOUBLE_EQ(g1[4].AsDouble(), 6.0);
    EXPECT_EQ(g2[1].AsInt(), 1) << "the seen-set is per group";
    EXPECT_EQ(g2[2].AsInt(), 5);
}

TEST(Executors, SortMultiKeyNullsLastAndStable) {
    Schema s({Col("a", TypeId::INT), Col("b", TypeId::INT), Col("tag", TypeId::TEXT)});
    std::vector<Row> rows = {{I(2), I(1), T("p")}, {N(), I(0), T("q")}, {I(1), I(5), T("r")},
                             {I(2), I(1), T("s")}, {I(1), I(9), T("t")}};
    SortExecutor sort(Vec(s, rows), {{ColRef(s, "", "a"), false}, {ColRef(s, "", "b"), true}});
    std::string order;
    for (auto& r : Drain(sort)) order += r[2].AsText();
    EXPECT_EQ(order, "trpsq");  // a asc, b desc, ties keep input order, NULL last
}

TEST(Executors, SortDescending) {
    Schema s({Col("a", TypeId::INT)});
    SortExecutor sort(Vec(s, {{I(1)}, {I(3)}, {I(2)}}), {{ColRef(s, "", "a"), true}});
    auto rows = Drain(sort);
    EXPECT_EQ(rows[0][0].AsInt(), 3);
    EXPECT_EQ(rows[2][0].AsInt(), 1);
}

TEST(Executors, LimitAndOffset) {
    Schema s({Col("a", TypeId::INT)});
    std::vector<Row> rows;
    for (int i = 0; i < 10; ++i) rows.push_back({I(i)});
    LimitExecutor l(Vec(s, rows), 3, 2);
    auto out = Drain(l);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0][0].AsInt(), 2);
    LimitExecutor zero(Vec(s, rows), 0, 0);
    EXPECT_TRUE(Drain(zero).empty());
    LimitExecutor past_end(Vec(s, rows), 5, 8);
    EXPECT_EQ(Drain(past_end).size(), 2u);
    LimitExecutor offset_only(Vec(s, rows), -1, 7);
    EXPECT_EQ(Drain(offset_only).size(), 3u);
}

TEST(Executors, LimitStopsPullingEarly) {
    Schema s({Col("a", TypeId::INT)});
    std::vector<Row> rows(1000, Row{I(1)});
    auto child = Vec(s, rows);
    auto* raw = child.get();
    LimitExecutor l(std::move(child), 5, 0);
    Drain(l);
    EXPECT_EQ(raw->ActualRows(), 5u);
}

TEST(Executors, Distinct) {
    Schema s({Col("a", TypeId::INT), Col("b", TypeId::TEXT)});
    DistinctExecutor d(Vec(s, {{I(1), T("x")}, {I(1), T("x")}, {I(1), T("y")}, {N(), T("x")}, {N(), T("x")}}));
    EXPECT_EQ(Drain(d).size(), 3u);
}

// ---- base table scans (need real storage, but no planner) ----

class ScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        table = catalog.CreateTable("t", {Col("k", TypeId::INT), Col("s", TypeId::TEXT)});
        for (int i = 0; i < 1000; ++i) {
            Row r = {i % 100 == 0 ? N() : I(i), T("row" + std::to_string(i))};
            table->heap->Insert(SerializeTuple(table->schema, r));
            ++table->row_count;
        }
        index = catalog.CreateIndex("t_k", "t", "k", false);
    }
    std::vector<int64_t> Keys(Executor& e) {
        std::vector<int64_t> out;
        for (auto& r : Drain(e)) out.push_back(r[0].AsInt());
        return out;
    }
    std::unique_ptr<IndexScanExecutor> IScan(std::optional<IndexBound> lo, std::optional<IndexBound> hi,
                                             ExprPtr pred = nullptr) {
        return std::make_unique<IndexScanExecutor>(table, index, "t", table->schema, lo, hi, pred);
    }

    TempFile file;
    DiskManager disk{file.path()};
    BufferPoolManager bpm{32, &disk};
    Catalog catalog{&bpm};
    TableInfo* table = nullptr;
    IndexInfo* index = nullptr;
};

TEST_F(ScanTest, SeqScanReturnsEveryRowWithRid) {
    SeqScanExecutor scan(table, "t", table->schema, nullptr);
    scan.Init();
    Row r;
    int n = 0;
    std::string buf;
    while (scan.Next(&r)) {
        ASSERT_TRUE(table->heap->Get(scan.CurrentRid(), &buf));
        EXPECT_EQ(DeserializeTuple(table->schema, buf.data(), buf.size())[1].AsText(), r[1].AsText());
        ++n;
    }
    EXPECT_EQ(n, 1000);
}

TEST_F(ScanTest, SeqScanWithPushedDownPredicate) {
    ExprPtr pred = BindExpr(Expr::Binary(BinOp::LT, Expr::Col("", "k"), Expr::Const(I(50))), table->schema);
    SeqScanExecutor scan(table, "t", table->schema, pred);
    EXPECT_EQ(Drain(scan).size(), 49u);  // 1..49 (0 is NULL)
}

TEST_F(ScanTest, IndexScanEquality) {
    auto s = IScan(IndexBound{42, true}, IndexBound{42, true});
    EXPECT_EQ(Keys(*s), std::vector<int64_t>{42});
}

TEST_F(ScanTest, IndexScanRangesRespectInclusivity) {
    EXPECT_EQ(Keys(*IScan(IndexBound{10, true}, IndexBound{13, true})), (std::vector<int64_t>{10, 11, 12, 13}));
    EXPECT_EQ(Keys(*IScan(IndexBound{10, false}, IndexBound{13, false})), (std::vector<int64_t>{11, 12}));
    EXPECT_EQ(Keys(*IScan(std::nullopt, IndexBound{3, true})), (std::vector<int64_t>{1, 2, 3}));
    EXPECT_EQ(Keys(*IScan(IndexBound{997, false}, std::nullopt)), (std::vector<int64_t>{998, 999}));
}

TEST_F(ScanTest, IndexScanFullAndEmptyRanges) {
    auto all = Keys(*IScan(std::nullopt, std::nullopt));
    EXPECT_EQ(all.size(), 990u) << "NULL keys are not in the index";
    EXPECT_TRUE(std::is_sorted(all.begin(), all.end()));
    EXPECT_TRUE(Keys(*IScan(IndexBound{500, true}, IndexBound{400, true})).empty());
    EXPECT_TRUE(Keys(*IScan(IndexBound{INT64_MAX, false}, std::nullopt)).empty());
}

TEST_F(ScanTest, IndexScanResidualPredicate) {
    ExprPtr pred = BindExpr(Expr::Binary(BinOp::EQ, Expr::Binary(BinOp::MOD, Expr::Col("", "k"), Expr::Const(I(2))),
                                         Expr::Const(I(0))),
                            table->schema);
    EXPECT_EQ(Keys(*IScan(IndexBound{1, true}, IndexBound{10, true}, pred)), (std::vector<int64_t>{2, 4, 6, 8, 10}));
}

TEST_F(ScanTest, ScansLeaveNoPinnedPages) {
    auto s = IScan(IndexBound{100, true}, IndexBound{900, true});
    Drain(*s);
    SeqScanExecutor seq(table, "t", table->schema, nullptr);
    Drain(seq);
    EXPECT_EQ(bpm.PinnedFrames(), 0u);
}

}  // namespace
}  // namespace minidb
