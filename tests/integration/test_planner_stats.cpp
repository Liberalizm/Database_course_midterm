// Integration tests: ANALYZE statistics in the planner's estimates, and index
// scans inside correlated subqueries.
#include <random>

#include "database.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

class PlannerStatsTest : public ::testing::Test {
protected:
    TempFile file;
    std::unique_ptr<Database> db = std::make_unique<Database>(file.path(), 128);

    std::vector<Row> Q(const std::string& sql) { return db->Execute(sql).back().rows; }
    int64_t Scalar(const std::string& sql) {
        auto rows = Q(sql);
        EXPECT_EQ(rows.size(), 1u) << sql;
        return rows.empty() || rows[0][0].IsNull() ? -1 : rows[0][0].AsInt();
    }
    std::string Plan(const std::string& sql) {
        std::string s;
        for (const Row& r : Q("EXPLAIN " + sql)) s += r[0].ToString() + "\n";
        return s;
    }
    // The planner's row estimate for the top node of a query.
    double Estimate(const std::string& sql) {
        std::string plan = Plan(sql);
        size_t p = plan.find("rows=");
        EXPECT_NE(p, std::string::npos) << plan;
        return std::stod(plan.substr(p + 5));
    }

    // orders: 5000 rows, 98% 'done' and 2% 'open'; amount = id; cust in 1..200.
    void SeedOrders() {
        db->Execute(
            "CREATE TABLE c (id INT PRIMARY KEY, name TEXT);"
            "CREATE TABLE o (id INT PRIMARY KEY, status TEXT, amount INT, cust INT)");
        db->Populate("c", 200);
        db->Populate("o", 5000);
        db->Execute(
            "UPDATE o SET status = CASE WHEN id % 50 = 0 THEN 'open' ELSE 'done' END, amount = id,"
            "             cust = id % 200 + 1;"
            "CREATE INDEX o_status ON o (status); CREATE INDEX o_cust ON o (cust)");
    }
};

TEST_F(PlannerStatsTest, AnalyzeMakesEstimatesMatchSkewedData) {
    SeedOrders();
    const std::string open = "SELECT * FROM o WHERE status = 'open'";
    const std::string done = "SELECT * FROM o WHERE status = 'done'";
    EXPECT_EQ(Estimate(open), Estimate(done)) << "without statistics every value looks alike";
    EXPECT_EQ(db->Execute("ANALYZE o").back().message, "ANALYZE");
    EXPECT_EQ(Estimate(open), 100);
    EXPECT_EQ(Estimate(done), 4900);
    // The index is worth it for the rare value only.
    EXPECT_NE(Plan(open).find("Index Scan using o_status"), std::string::npos) << Plan(open);
    EXPECT_NE(Plan(done).find("Seq Scan on o"), std::string::npos) << Plan(done);
    EXPECT_EQ(Q(open).size(), 100u);
}

TEST_F(PlannerStatsTest, RangesNullsGroupsAndJoins) {
    SeedOrders();
    db->Execute("UPDATE o SET status = NULL WHERE id <= 1000; ANALYZE");
    EXPECT_NEAR(Estimate("SELECT * FROM o WHERE amount BETWEEN 1001 AND 2000"), 1000, 60);
    EXPECT_NEAR(Estimate("SELECT * FROM o WHERE amount > 4000"), 1000, 60);
    EXPECT_NEAR(Estimate("SELECT * FROM o WHERE amount < 0"), 1, 1);
    EXPECT_NEAR(Estimate("SELECT * FROM o WHERE status IS NULL"), 1000, 1);
    EXPECT_NEAR(Estimate("SELECT * FROM o WHERE status IS NOT NULL"), 4000, 1);
    EXPECT_EQ(Estimate("SELECT status, COUNT(*) FROM o GROUP BY status"), 3) << "done, open and NULL";
    EXPECT_EQ(Estimate("SELECT cust, COUNT(*) FROM o GROUP BY cust"), 200);
    EXPECT_NEAR(Estimate("SELECT * FROM o JOIN c ON o.cust = c.id"), 5000, 1);
    EXPECT_NEAR(Estimate("SELECT * FROM o WHERE cust IN (1, 2, 3)"), 75, 1);
}

TEST_F(PlannerStatsTest, StatisticsPersistAndFollowSchemaChanges) {
    SeedOrders();
    db->Execute("ANALYZE o");
    EXPECT_FALSE(db->GetCatalog().RequireTable("o")->stats.empty());
    EXPECT_TRUE(db->GetCatalog().RequireTable("c")->stats.empty()) << "only the named table";
    db.reset();
    db = std::make_unique<Database>(file.path(), 128);
    TableInfo* o = db->GetCatalog().RequireTable("o");
    ASSERT_EQ(o->stats.size(), 4u) << "kept in the catalog";
    EXPECT_EQ(o->stats[1].distinct, 2);
    EXPECT_EQ(Estimate("SELECT * FROM o WHERE status = 'open'"), 100);

    db->Execute("ALTER TABLE o RENAME COLUMN status TO state");
    EXPECT_EQ(Estimate("SELECT * FROM o WHERE state = 'open'"), 100) << "a rename keeps them";
    db->Execute("ALTER TABLE o ADD COLUMN note TEXT");
    EXPECT_TRUE(o->stats.empty()) << "new columns: the statistics are outdated";
}

TEST_F(PlannerStatsTest, AnalyzeIsTransactional) {
    SeedOrders();
    db->Execute("BEGIN; ANALYZE; ROLLBACK");
    EXPECT_TRUE(db->GetCatalog().RequireTable("o")->stats.empty());
    db->Execute("BEGIN; ANALYZE; COMMIT");
    EXPECT_FALSE(db->GetCatalog().RequireTable("o")->stats.empty());
    EXPECT_THROW(db->Execute("ANALYZE nope"), DbError);
    db->Execute("CREATE VIEW v AS SELECT * FROM o");
    EXPECT_THROW(db->Execute("ANALYZE v"), DbError);
}

TEST_F(PlannerStatsTest, CorrelatedSubqueriesUseIndexes) {
    SeedOrders();
    const std::string q = "SELECT COUNT(*) FROM c WHERE EXISTS (SELECT 1 FROM o WHERE o.cust = c.id AND o.amount > 4900)";
    // The plan of the subquery is not shown by EXPLAIN, so check it ran fast
    // through the index: few rows read per outer row.
    db->Execute("ANALYZE");
    EXPECT_EQ(Scalar(q), 100);
    // Same results as with the condition hidden from the index (o.cust + 0).
    for (const char* cond : {"o.cust = c.id", "o.cust < c.id", "o.cust >= c.id + 0.5", "o.cust = c.id + 0.5",
                             "o.cust <= c.id - 0.5", "c.id > o.cust"}) {
        std::string a = std::string("SELECT SUM((SELECT COUNT(*) FROM o WHERE ") + cond + ")) FROM c WHERE c.id <= 20";
        std::string b = a;
        b.replace(b.find("o.cust"), 6, "o.cust + 0");
        EXPECT_EQ(Scalar(a), Scalar(b)) << cond;
    }
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM c WHERE (SELECT COUNT(*) FROM o WHERE o.cust = c.id) = 25"), 200);
}

TEST_F(PlannerStatsTest, ParameterizedScanEdgeCases) {
    db->Execute(
        "CREATE TABLE p (id INT PRIMARY KEY, k INT, t TEXT, d DATE);"
        "CREATE INDEX p_k ON p (k); CREATE INDEX p_t ON p (t); CREATE INDEX p_d ON p (d);"
        "CREATE TABLE q (id INT, k DOUBLE, t TEXT, d TIMESTAMP)");
    db->Populate("p", 3000);
    db->Execute(
        "UPDATE p SET k = id % 100, t = 'v' || CAST(id % 100 AS TEXT), d = DATE '2024-01-01' + id % 100;"
        "INSERT INTO q VALUES (1, 5, 'v5', '2024-01-06 00:00:00'), (2, 5.5, 'v7', '2024-01-06 12:00:00'),"
        "                     (3, NULL, NULL, NULL), (4, -1, 'nope', '2023-01-01 00:00:00')");
    auto per_row = [&](const std::string& cond) {
        std::vector<int64_t> out;
        for (const Row& r : Q("SELECT (SELECT COUNT(*) FROM p WHERE " + cond + ") FROM q ORDER BY id"))
            out.push_back(r[0].AsInt());
        return out;
    };
    EXPECT_EQ(per_row("p.k = q.k"), (std::vector<int64_t>{30, 0, 0, 0})) << "5.5 is no INT; NULL matches nothing";
    EXPECT_EQ(per_row("p.k > q.k"), (std::vector<int64_t>{94 * 30, 94 * 30, 0, 3000}));
    EXPECT_EQ(per_row("p.k <= q.k"), (std::vector<int64_t>{6 * 30, 6 * 30, 0, 0}));
    EXPECT_EQ(per_row("p.t = q.t"), (std::vector<int64_t>{30, 30, 0, 0})) << "TEXT index (hashed)";
    EXPECT_EQ(per_row("p.d = q.d"), (std::vector<int64_t>{30, 0, 0, 0})) << "DATE against TIMESTAMP";
    EXPECT_EQ(per_row("p.d < q.d"), (std::vector<int64_t>{5 * 30, 6 * 30, 0, 0}));
    EXPECT_THROW(Q("SELECT (SELECT COUNT(*) FROM p WHERE p.k = q.t) FROM q"), DbError)
        << "an incomparable outer value is still an error";
}

// Randomized: correlated subqueries give the same answers with and without
// an index on the inner column.
TEST_F(PlannerStatsTest, RandomCorrelatedSubqueriesMatchUnindexed) {
    db->Execute("CREATE TABLE a (x INT, y INT); CREATE TABLE b (x INT, y INT); CREATE TABLE b2 (x INT, y INT)");
    std::mt19937 rng(7);
    for (int i = 0; i < 300; ++i) {
        auto v = [&] { return rng() % 10 == 0 ? std::string("NULL") : std::to_string(rng() % 40); };
        std::string row = "(" + v() + ", " + v() + ")";
        db->Execute("INSERT INTO b VALUES " + row + "; INSERT INTO b2 VALUES " + row);
        if (i < 60) db->Execute("INSERT INTO a VALUES (" + v() + ", " + v() + ")");
    }
    db->Execute("CREATE INDEX b_x ON b (x); ANALYZE");
    const char* ops[] = {"=", "<", "<=", ">", ">="};
    for (const char* op : ops) {
        for (const char* rhs : {"a.x", "a.y + 1", "a.x / 2.0"}) {
            std::string cond = std::string("b.x ") + op + " " + rhs;
            std::string with = "SELECT a.x, a.y, (SELECT COUNT(*) FROM b WHERE " + cond +
                               "), EXISTS (SELECT 1 FROM b WHERE " + cond + " AND b.y > 20) FROM a ORDER BY 1, 2, 3, 4";
            std::string without = with;
            for (size_t p; (p = without.find("FROM b WHERE b.x")) != std::string::npos;)
                without.replace(p, 16, "FROM b2 b WHERE b.x");
            auto r1 = Q(with), r2 = Q(without);
            ASSERT_EQ(r1.size(), r2.size()) << cond;
            for (size_t i = 0; i < r1.size(); ++i)
                for (size_t j = 0; j < r1[i].size(); ++j)
                    ASSERT_EQ(r1[i][j].ToString(), r2[i][j].ToString()) << cond << " row " << i;
        }
    }
}

}  // namespace
}  // namespace minidb
