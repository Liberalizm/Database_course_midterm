// Integration tests: DATE and TIMESTAMP columns end to end (SQL in, rows out).
#include "database.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

class TemporalTest : public ::testing::Test {
protected:
    TempFile file;
    std::unique_ptr<Database> db = std::make_unique<Database>(file.path(), 64);

    std::vector<Row> Q(const std::string& sql) { return db->Execute(sql).back().rows; }
    std::string One(const std::string& sql) {
        auto rows = Q(sql);
        EXPECT_EQ(rows.size(), 1u) << sql;
        return rows.empty() ? "" : rows[0][0].ToString();
    }
    std::string Plan(const std::string& sql) {
        std::string s;
        for (const Row& r : Q("EXPLAIN " + sql)) s += r[0].ToString() + "\n";
        return s;
    }
    void Seed() {
        db->Execute(
            "CREATE TABLE ev (id INT PRIMARY KEY, d DATE, ts TIMESTAMP);"
            "INSERT INTO ev VALUES (1, '2024-02-28', '2024-02-28 23:59:59'),"
            "                      (2, DATE '2024-02-29', TIMESTAMP '2024-02-29 00:00:00'),"
            "                      (3, '2023-12-31', '2023-12-31T12:30'),"
            "                      (4, NULL, NULL);");
    }
};

TEST_F(TemporalTest, StoresAndPrintsValues) {
    Seed();
    auto rows = Q("SELECT d, ts FROM ev ORDER BY id");
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0][0].Type(), TypeId::DATE);
    EXPECT_EQ(rows[0][1].Type(), TypeId::TIMESTAMP);
    EXPECT_EQ(rows[2][0].ToString(), "2023-12-31");
    EXPECT_EQ(rows[2][1].ToString(), "2023-12-31 12:30:00");
    EXPECT_TRUE(rows[3][0].IsNull());
}

TEST_F(TemporalTest, InvalidValuesAreRejected) {
    Seed();
    EXPECT_THROW(db->Execute("INSERT INTO ev VALUES (5, '2023-02-29', NULL)"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO ev VALUES (5, 'soon', NULL)"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO ev VALUES (5, 20240101, NULL)"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO ev VALUES (5, NULL, '2024-01-01 25:00')"), DbError);
    EXPECT_THROW(db->Execute("SELECT DATE '2024-02-30'"), DbError);
    EXPECT_THROW(db->Execute("UPDATE ev SET d = d + 3000000"), DbError) << "past year 9999";
    EXPECT_EQ(One("SELECT COUNT(*) FROM ev"), "4");
    EXPECT_EQ(One("SELECT d FROM ev WHERE id = 1"), "2024-02-28") << "the failed UPDATE changed nothing";
}

TEST_F(TemporalTest, ComparesWithLiteralsAndText) {
    Seed();
    EXPECT_EQ(One("SELECT COUNT(*) FROM ev WHERE d >= '2024-01-01'"), "2");
    EXPECT_EQ(One("SELECT COUNT(*) FROM ev WHERE d BETWEEN DATE '2024-02-01' AND DATE '2024-02-28'"), "1");
    EXPECT_EQ(One("SELECT COUNT(*) FROM ev WHERE ts < d + 1"), "3");
    EXPECT_EQ(One("SELECT COUNT(*) FROM ev WHERE ts = d"), "1") << "a DATE equals its midnight";
    EXPECT_EQ(One("SELECT id FROM ev WHERE ts > TIMESTAMP '2024-02-28 23:59:58' AND ts < '2024-02-29'"), "1");
    EXPECT_THROW(db->Execute("SELECT * FROM ev WHERE d > 5"), DbError);
}

TEST_F(TemporalTest, ArithmeticAndFunctions) {
    Seed();
    EXPECT_EQ(One("SELECT d + 1 FROM ev WHERE id = 1"), "2024-02-29");
    EXPECT_EQ(One("SELECT DATE '2024-01-01' - d FROM ev WHERE id = 3"), "1");
    EXPECT_EQ(One("SELECT ts - TIMESTAMP '2024-02-28 00:00:00' FROM ev WHERE id = 1"), "86399");
    EXPECT_EQ(One("SELECT year(d) * 10000 + month(d) * 100 + day(d) FROM ev WHERE id = 2"), "20240229");
    EXPECT_EQ(One("SELECT hour(ts) * 60 + minute(ts) FROM ev WHERE id = 3"), "750");
    EXPECT_EQ(One("SELECT date(ts) FROM ev WHERE id = 1"), "2024-02-28");
    EXPECT_EQ(One("SELECT CAST('2024-07-04' AS DATE) + 1"), "2024-07-05");
    EXPECT_EQ(One("SELECT CAST(d AS TEXT) || '!' FROM ev WHERE id = 1"), "2024-02-28!");
    EXPECT_EQ(One("SELECT CAST('12' AS INT) * 2"), "24");
    EXPECT_EQ(One("SELECT COUNT(*) FROM ev WHERE year(d) IS NULL"), "1");
}

TEST_F(TemporalTest, OrderGroupAndAggregate) {
    Seed();
    db->Execute("INSERT INTO ev VALUES (5, '2024-02-28', '2024-02-28 08:00:00')");
    auto rows = Q("SELECT d, COUNT(*) FROM ev WHERE d IS NOT NULL GROUP BY d ORDER BY d DESC");
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0].ToString(), "2024-02-29");
    EXPECT_EQ(rows[1][0].ToString(), "2024-02-28");
    EXPECT_EQ(rows[1][1].AsInt(), 2);
    EXPECT_EQ(One("SELECT MIN(d) FROM ev"), "2023-12-31");
    EXPECT_EQ(One("SELECT MAX(ts) FROM ev"), "2024-02-29 00:00:00");
    EXPECT_EQ(One("SELECT COUNT(DISTINCT d) FROM ev"), "3");
    EXPECT_THROW(db->Execute("SELECT SUM(d) FROM ev"), DbError);
}

TEST_F(TemporalTest, JoinsDateWithTimestamp) {
    Seed();
    db->Execute("CREATE TABLE days (day DATE, label TEXT); INSERT INTO days VALUES ('2024-02-29', 'leap')");
    // Hash join keys must agree with comparison: DATE '2024-02-29' = TIMESTAMP '2024-02-29 00:00:00'.
    EXPECT_EQ(One("SELECT e.id FROM ev e JOIN days ON e.ts = days.day"), "2");
}

TEST_F(TemporalTest, IndexesAndKeys) {
    db->Execute("CREATE TABLE logs (id INT PRIMARY KEY, day DATE, at TIMESTAMP UNIQUE)");
    db->Populate("logs", 2000);
    db->Execute("CREATE INDEX logs_day ON logs (day)");
    const std::string day = One("SELECT day FROM logs WHERE id = 1000");
    std::string plan = Plan("SELECT COUNT(*) FROM logs WHERE day = DATE '" + day + "'");
    EXPECT_NE(plan.find("Index Scan using logs_day"), std::string::npos) << plan;
    EXPECT_NE(plan.find("DATE '" + day + "'"), std::string::npos) << "bounds print as dates: " << plan;
    EXPECT_EQ(One("SELECT COUNT(*) FROM logs WHERE day = DATE '" + day + "'"),
              One("SELECT COUNT(*) FROM logs WHERE CAST(day AS TEXT) = '" + day + "'"));
    EXPECT_EQ(One("SELECT COUNT(*) FROM logs WHERE day >= DATE '2022-01-01' AND day < DATE '2023-01-01'"),
              One("SELECT COUNT(*) FROM logs WHERE year(day) = 2022"));

    db->Execute("CREATE TABLE u (at TIMESTAMP PRIMARY KEY)");
    db->Execute("INSERT INTO u VALUES ('2024-01-01 10:00:00')");
    EXPECT_THROW(db->Execute("INSERT INTO u VALUES (TIMESTAMP '2024-01-01 10:00:00')"), DbError);
    EXPECT_NO_THROW(db->Execute("INSERT INTO u VALUES ('2024-01-01 10:00:01')"));
}

TEST_F(TemporalTest, DefaultsAlterAndPersistence) {
    db->Execute(
        "CREATE TABLE t (id INT, d DATE DEFAULT DATE '2000-01-01');"
        "INSERT INTO t (id) VALUES (1);"
        "ALTER TABLE t ADD COLUMN seen TIMESTAMP DEFAULT '2001-02-03 04:05:06'");
    db.reset();
    db = std::make_unique<Database>(file.path(), 64);
    auto rows = Q("SELECT d, seen FROM t");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].ToSqlLiteral(), "DATE '2000-01-01'");
    EXPECT_EQ(rows[0][1].ToSqlLiteral(), "TIMESTAMP '2001-02-03 04:05:06'");
    db->Execute("INSERT INTO t (id) VALUES (2)");
    EXPECT_EQ(One("SELECT d FROM t WHERE id = 2"), "2000-01-01") << "DEFAULT survived the reopen";
}

TEST_F(TemporalTest, DateIsStillAUsableColumnName) {
    db->Execute("CREATE TABLE cal (date DATE, timestamp TIMESTAMP)");
    db->Execute("INSERT INTO cal VALUES ('2024-01-01', '2024-01-01 00:00:00')");
    EXPECT_EQ(One("SELECT date FROM cal WHERE date = DATE '2024-01-01'"), "2024-01-01");
    EXPECT_EQ(One("SELECT date(timestamp) FROM cal"), "2024-01-01");
}

}  // namespace
}  // namespace minidb
