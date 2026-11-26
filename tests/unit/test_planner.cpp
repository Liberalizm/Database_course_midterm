// Unit tests: Planner (plan shape and optimizer decisions).
// The planner is exercised directly: SelectStmt in, operator tree out.
#include <algorithm>

#include "database.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;
using testing::Drain;

class PlannerTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        file_ = new TempFile();
        db_ = new Database(file_->path(), 256);
        db_->Execute(
            "CREATE TABLE big (id INT PRIMARY KEY, grp INT, label TEXT);"
            "CREATE TABLE mid (id INT PRIMARY KEY, big_id INT);"
            "CREATE TABLE small (id INT PRIMARY KEY, name TEXT);"
            "CREATE TABLE tiny (id INT PRIMARY KEY, v INT);"
            "INSERT INTO tiny VALUES (1, 10), (2, 20), (3, 30);"
            "CREATE INDEX big_grp ON big (grp);");
        db_->Populate("big", 20000);
        db_->Populate("mid", 2000);
        db_->Populate("small", 50);
    }
    static void TearDownTestSuite() {
        delete db_;
        delete file_;
    }

    static SelectStmt Parse(const std::string& sql) {
        auto stmts = Parser(sql).ParseAll();
        return std::get<SelectStmt>(stmts.at(0));
    }
    static ExecPtr PlanOf(const std::string& sql) {
        Planner p(&db_->GetCatalog());
        return p.PlanSelect(Parse(sql));
    }
    static std::string ExplainOf(const std::string& sql) {
        std::string out;
        for (auto& line : Planner::Explain(*PlanOf(sql), false)) out += line + "\n";
        return out;
    }
    static bool Has(const std::string& plan, const std::string& what) { return plan.find(what) != std::string::npos; }
    static size_t Rows(const std::string& sql) { return Drain(*PlanOf(sql)).size(); }

    static inline TempFile* file_ = nullptr;
    static inline Database* db_ = nullptr;
};

// ---- access path selection ----

TEST_F(PlannerTest, PointLookupUsesPrimaryKeyIndex) {
    auto plan = ExplainOf("SELECT * FROM big WHERE id = 1234");
    EXPECT_TRUE(Has(plan, "Index Scan using big_pkey")) << plan;
    EXPECT_TRUE(Has(plan, "Index Cond: big.id = 1234")) << plan;
    EXPECT_EQ(Rows("SELECT * FROM big WHERE id = 1234"), 1u);
}

TEST_F(PlannerTest, ConstantOnTheLeftAlsoUsesIndex) {
    EXPECT_TRUE(Has(ExplainOf("SELECT * FROM big WHERE 1234 = id"), "Index Scan"));
    EXPECT_TRUE(Has(ExplainOf("SELECT * FROM big WHERE 19990 < id"), "Index Cond: big.id > 19990"));
}

TEST_F(PlannerTest, TinyTablePrefersSeqScan) {
    auto plan = ExplainOf("SELECT * FROM tiny WHERE id = 2");
    EXPECT_TRUE(Has(plan, "Seq Scan on tiny")) << "an index is not worth it for a 1-page table\n" << plan;
}

TEST_F(PlannerTest, SelectiveRangeUsesIndexWideRangeDoesNot) {
    EXPECT_TRUE(Has(ExplainOf("SELECT * FROM big WHERE id BETWEEN 100 AND 199"), "Index Scan"));
    EXPECT_TRUE(Has(ExplainOf("SELECT * FROM big WHERE id > 100"), "Seq Scan"));
}

TEST_F(PlannerTest, RangeBoundsAreTightened) {
    auto plan = ExplainOf("SELECT * FROM big WHERE id > 5 AND id >= 10 AND id < 30 AND id <= 40");
    EXPECT_TRUE(Has(plan, "Index Cond: big.id >= 10 AND big.id < 30")) << plan;
    EXPECT_EQ(Rows("SELECT * FROM big WHERE id > 5 AND id >= 10 AND id < 30 AND id <= 40"), 20u);
}

TEST_F(PlannerTest, NonIndexedPredicateBecomesScanFilter) {
    auto plan = ExplainOf("SELECT * FROM big WHERE label = 'x'");
    EXPECT_TRUE(Has(plan, "Seq Scan on big  Filter: big.label = 'x'")) << plan;
    EXPECT_FALSE(Has(plan, "\n    -> Filter")) << "filter is pushed into the scan\n" << plan;
}

TEST_F(PlannerTest, SecondaryIndexAndResidualFilter) {
    auto plan = ExplainOf("SELECT * FROM big WHERE grp = 7 AND label LIKE 'a%'");
    EXPECT_TRUE(Has(plan, "Index Scan using big_grp")) << plan;
    EXPECT_TRUE(Has(plan, "Filter: big.label LIKE 'a%'")) << plan;
}

TEST_F(PlannerTest, IndexOnlyUsedForIntegerConstants) {
    EXPECT_TRUE(Has(ExplainOf("SELECT * FROM big WHERE id = 5.0"), "Seq Scan"));
    EXPECT_TRUE(Has(ExplainOf("SELECT * FROM big WHERE id + 0 = 5"), "Seq Scan"));
}

TEST_F(PlannerTest, EstimatesForUniqueLookupAreOneRow) {
    auto plan = PlanOf("SELECT * FROM big WHERE id = 77");
    const Executor* scan = plan->Children().at(0);
    EXPECT_DOUBLE_EQ(scan->est_rows, 1.0);
}

// ---- joins ----

TEST_F(PlannerTest, EquiJoinUsesHashJoinWithSmallerBuildSide) {
    auto plan = PlanOf("SELECT * FROM big b JOIN small s ON b.grp = s.id");
    const Executor* join = plan->Children().at(0);
    ASSERT_TRUE(Has(join->Describe(), "Hash Join")) << join->Describe();
    auto kids = join->Children();
    EXPECT_TRUE(Has(kids[0]->Describe(), "on big")) << "probe = big";
    EXPECT_TRUE(Has(kids[1]->Describe(), "on small")) << "build = small";
}

TEST_F(PlannerTest, NonEquiJoinUsesNestedLoop) {
    EXPECT_TRUE(Has(ExplainOf("SELECT * FROM small a, small b WHERE a.id < b.id"), "Nested Loop Join"));
    EXPECT_EQ(Rows("SELECT * FROM small a, small b WHERE a.id < b.id"), 50u * 49u / 2u);
}

TEST_F(PlannerTest, CrossJoinWithoutPredicate) {
    EXPECT_TRUE(Has(ExplainOf("SELECT * FROM tiny a, tiny b"), "cross product"));
    EXPECT_EQ(Rows("SELECT * FROM tiny a, tiny b"), 9u);
}

TEST_F(PlannerTest, SingleTablePredicatesArePushedBelowJoin) {
    auto plan = ExplainOf("SELECT * FROM big b JOIN small s ON b.grp = s.id WHERE s.name LIKE 'a%' AND b.label <> 'z'");
    EXPECT_TRUE(Has(plan, "Seq Scan on small s  Filter: s.name LIKE 'a%'")) << plan;
    EXPECT_TRUE(Has(plan, "Filter: b.label <> 'z'")) << plan;
    EXPECT_FALSE(Has(plan, "Join Filter")) << plan;
}

TEST_F(PlannerTest, JoinOrderStartsWithSmallestInputAndAvoidsCrossProducts) {
    // FROM order: big, mid, small; join graph: big - mid - small (via mid.big_id, mid.id)
    auto plan = ExplainOf(
        "SELECT * FROM big b, mid m, small s WHERE m.big_id = b.id AND s.id = m.id");
    EXPECT_FALSE(Has(plan, "Nested Loop")) << plan;
    EXPECT_EQ(std::count(plan.begin(), plan.end(), '\n'), 6) << plan;  // projection + 2 joins + 3 scans
    // The result must not depend on how the query is written.
    EXPECT_EQ(Rows("SELECT * FROM big b, mid m, small s WHERE m.big_id = b.id AND s.id = m.id"),
              Rows("SELECT * FROM small s JOIN mid m ON m.id = s.id JOIN big b ON b.id = m.big_id"));
}

TEST_F(PlannerTest, StarExpansionKeepsFromOrderEvenIfJoinIsReordered) {
    auto plan = PlanOf("SELECT * FROM big b JOIN tiny t ON b.id = t.id");
    const Schema& out = plan->OutSchema();
    ASSERT_EQ(out.Size(), 5u);
    EXPECT_EQ(out.At(0).name, "id");
    EXPECT_EQ(out.At(2).name, "label");
    EXPECT_EQ(out.At(4).name, "v");
}

// ---- aggregation, sorting, limits ----

TEST_F(PlannerTest, PipelineOrder) {
    auto plan = ExplainOf(
        "SELECT DISTINCT grp, COUNT(*) AS n FROM big WHERE id < 1000 GROUP BY grp HAVING COUNT(*) > 1 "
        "ORDER BY n DESC LIMIT 5");
    std::vector<std::string> order = {"Limit", "HashDistinct", "Projection", "Sort", "Having", "HashAggregate", "Scan"};
    size_t pos = 0;
    for (const auto& op : order) {
        size_t found = plan.find(op, pos);
        ASSERT_NE(found, std::string::npos) << op << " missing or out of order\n" << plan;
        pos = found;
    }
}

TEST_F(PlannerTest, AggregateWithoutGroupBy) {
    auto plan = ExplainOf("SELECT COUNT(*), MAX(id) FROM big");
    EXPECT_TRUE(Has(plan, "Aggregate  Aggs: count(*), max(big.id)")) << plan;
    auto rows = Drain(*PlanOf("SELECT COUNT(*), MAX(id) FROM big"));
    EXPECT_EQ(rows[0][0].AsInt(), 20000);
}

TEST_F(PlannerTest, SameAggregateIsComputedOnce) {
    auto plan = ExplainOf("SELECT COUNT(*), COUNT(*) + 1 FROM big HAVING COUNT(*) > 0 ORDER BY COUNT(*)");
    EXPECT_TRUE(Has(plan, "Aggs: count(*)  ")) << plan;
}

TEST_F(PlannerTest, OrderByAliasAndPosition) {
    auto rows = Drain(*PlanOf("SELECT id AS k FROM tiny ORDER BY k DESC"));
    EXPECT_EQ(rows[0][0].AsInt(), 3);
    rows = Drain(*PlanOf("SELECT v, id FROM tiny ORDER BY 2 DESC"));
    EXPECT_EQ(rows[0][1].AsInt(), 3);
    rows = Drain(*PlanOf("SELECT id FROM tiny ORDER BY v DESC"));  // column not in select list
    EXPECT_EQ(rows[0][0].AsInt(), 3);
}

TEST_F(PlannerTest, OutputColumnNames) {
    auto plan = PlanOf("SELECT id, v * 2, v AS vee, COUNT(*) FROM tiny GROUP BY id, v * 2, v");
    const Schema& s = plan->OutSchema();
    EXPECT_EQ(s.At(0).name, "id");
    EXPECT_EQ(s.At(1).name, "v * 2") << "header shows the expression as written";
    EXPECT_EQ(s.At(2).name, "vee");
    EXPECT_EQ(s.At(3).name, "count(*)");
}

TEST_F(PlannerTest, SelectWithoutFrom) {
    EXPECT_TRUE(Has(ExplainOf("SELECT 1 + 1"), "Result"));
    EXPECT_EQ(Drain(*PlanOf("SELECT 1 + 1"))[0][0].AsInt(), 2);
    EXPECT_TRUE(Drain(*PlanOf("SELECT 1 WHERE 1 = 2")).empty());
}

// ---- semantic errors are detected at plan time ----

TEST_F(PlannerTest, SemanticErrors) {
    const char* bad[] = {
        "SELECT * FROM nosuchtable",
        "SELECT nosuchcol FROM tiny",
        "SELECT id FROM tiny a, tiny b",                // ambiguous
        "SELECT x.id FROM tiny t",                      // unknown alias
        "SELECT * FROM tiny t, small t",                // duplicate alias
        "SELECT id, COUNT(*) FROM tiny",                // id not grouped
        "SELECT * FROM tiny GROUP BY id",               // star with group by
        "SELECT id FROM tiny WHERE COUNT(*) > 1",       // aggregate in WHERE
        "SELECT id FROM tiny HAVING id > 1",            // HAVING without aggregation
        "SELECT id FROM tiny ORDER BY 5",               // position out of range
        "SELECT SUM(COUNT(*)) FROM tiny",               // nested aggregate
        "SELECT *",                                     // star without FROM
        "SELECT z.* FROM tiny",                         // unknown star table
    };
    for (const char* sql : bad) EXPECT_THROW(PlanOf(sql), DbError) << sql;
}

// ---- DML access paths ----

TEST_F(PlannerTest, DmlScanUsesIndex) {
    Planner p(&db_->GetCatalog());
    TableInfo* big = db_->GetCatalog().RequireTable("big");
    auto scan = p.PlanTableScan(big, Expr::Binary(BinOp::EQ, Expr::Col("", "id"), Expr::Const(Value::Int(10))));
    EXPECT_TRUE(Has(scan->Describe(), "Index Scan")) << scan->Describe();
    auto full = p.PlanTableScan(big, nullptr);
    EXPECT_TRUE(Has(full->Describe(), "Seq Scan"));
    EXPECT_THROW(p.PlanTableScan(big, Expr::Agg(AggFunc::COUNT, nullptr, true)), DbError);
}

}  // namespace
}  // namespace minidb
