// Integration tests: the whole engine through the Database API (SQL in, rows out).
#include <map>
#include <random>

#include "database.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

class SqlTest : public ::testing::Test {
protected:
    TempFile file;
    std::unique_ptr<Database> db = std::make_unique<Database>(file.path(), 64);

    std::vector<Row> Q(const std::string& sql) { return db->Execute(sql).back().rows; }
    std::string Msg(const std::string& sql) { return db->Execute(sql).back().message; }
    int64_t Scalar(const std::string& sql) {
        auto rows = Q(sql);
        EXPECT_EQ(rows.size(), 1u) << sql;
        return rows.at(0).at(0).AsInt();
    }

    void SeedCompany() {
        db->Execute(
            "CREATE TABLE dept (id INT PRIMARY KEY, name TEXT);"
            "CREATE TABLE emp (id INT PRIMARY KEY, name TEXT, dept_id INT, salary DOUBLE);"
            "INSERT INTO dept VALUES (1, 'eng'), (2, 'sales'), (3, 'empty');"
            "INSERT INTO emp VALUES (1, 'ann', 1, 100.0), (2, 'bob', 1, 80), (3, 'cid', 2, 50),"
            "                       (4, 'dan', 2, 70), (5, 'eve', NULL, 60);");
    }

    // All indexes of a table must contain exactly one entry per non-NULL key.
    void ExpectIndexesConsistent(const std::string& table) {
        TableInfo* t = db->GetCatalog().RequireTable(table);
        for (IndexInfo* idx : db->GetCatalog().IndexesOf(table)) {
            int64_t non_null = Scalar("SELECT COUNT(" + idx->column + ") FROM " + table);
            EXPECT_EQ(static_cast<int64_t>(idx->tree->CheckAndCount()), non_null) << idx->name;
        }
        EXPECT_EQ(t->row_count, Scalar("SELECT COUNT(*) FROM " + table)) << "row_count statistic drifted";
    }
};

TEST_F(SqlTest, StatementMessages) {
    EXPECT_EQ(Msg("CREATE TABLE t (a INT)"), "CREATE TABLE");
    EXPECT_EQ(Msg("CREATE INDEX t_a ON t (a)"), "CREATE INDEX");
    EXPECT_EQ(Msg("INSERT INTO t VALUES (1), (2), (3)"), "INSERT 3");
    EXPECT_EQ(Msg("UPDATE t SET a = a + 10 WHERE a > 1"), "UPDATE 2");
    EXPECT_EQ(Msg("DELETE FROM t WHERE a = 1"), "DELETE 1");
    EXPECT_EQ(Msg("SELECT * FROM t"), "(2 rows)");
    EXPECT_EQ(Msg("DROP INDEX t_a"), "DROP INDEX");
    EXPECT_EQ(Msg("DROP TABLE t"), "DROP TABLE");
}

TEST_F(SqlTest, FiltersAndOperators) {
    SeedCompany();
    EXPECT_EQ(Q("SELECT name FROM emp WHERE salary > 65 AND dept_id = 1").size(), 2u);
    EXPECT_EQ(Q("SELECT name FROM emp WHERE dept_id IS NULL")[0][0].AsText(), "eve");
    EXPECT_EQ(Q("SELECT name FROM emp WHERE name LIKE '%n'").size(), 2u);
    EXPECT_EQ(Q("SELECT name FROM emp WHERE id IN (1, 3, 99)").size(), 2u);
    EXPECT_EQ(Q("SELECT name FROM emp WHERE salary BETWEEN 60 AND 80").size(), 3u);
    EXPECT_EQ(Q("SELECT name FROM emp WHERE NOT (dept_id = 1)").size(), 2u) << "NULL row excluded";
    EXPECT_EQ(Scalar("SELECT 1 + 2 * 3"), 7);
}

TEST_F(SqlTest, Joins) {
    SeedCompany();
    auto j = Q("SELECT e.name, d.name FROM emp e JOIN dept d ON e.dept_id = d.id ORDER BY e.name");
    ASSERT_EQ(j.size(), 4u);
    EXPECT_EQ(j[0][0].AsText(), "ann");
    EXPECT_EQ(j[0][1].AsText(), "eng");
    EXPECT_EQ(Q("SELECT * FROM emp, dept").size(), 15u);
    EXPECT_EQ(Q("SELECT e.name FROM emp e, emp m WHERE e.dept_id = m.dept_id AND e.id < m.id").size(), 2u);
}

TEST_F(SqlTest, Aggregation) {
    SeedCompany();
    auto g = Q("SELECT d.name, COUNT(*) AS n, AVG(e.salary) FROM emp e, dept d WHERE e.dept_id = d.id "
               "GROUP BY d.name HAVING COUNT(*) >= 2 ORDER BY n DESC, d.name");
    ASSERT_EQ(g.size(), 2u);
    EXPECT_EQ(g[0][0].AsText(), "eng");
    EXPECT_DOUBLE_EQ(g[0][2].AsDouble(), 90.0);

    auto a = Q("SELECT COUNT(*), COUNT(dept_id), SUM(salary), MIN(name), MAX(id) FROM emp")[0];
    EXPECT_EQ(a[0].AsInt(), 5);
    EXPECT_EQ(a[1].AsInt(), 4);
    EXPECT_DOUBLE_EQ(a[2].AsDouble(), 360.0);
    EXPECT_EQ(a[3].AsText(), "ann");
    EXPECT_EQ(a[4].AsInt(), 5);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp WHERE id > 100"), 0);
    EXPECT_EQ(Q("SELECT dept_id, COUNT(*) FROM emp GROUP BY dept_id").size(), 3u) << "NULL forms its own group";
}

TEST_F(SqlTest, SortDistinctLimit) {
    SeedCompany();
    EXPECT_EQ(Q("SELECT DISTINCT dept_id FROM emp").size(), 3u);
    EXPECT_EQ(Q("SELECT id FROM emp ORDER BY salary DESC LIMIT 2 OFFSET 1")[0][0].AsInt(), 2);
    auto r = Q("SELECT dept_id FROM emp ORDER BY dept_id");
    EXPECT_TRUE(r.back()[0].IsNull()) << "NULLs sort last";
}

TEST_F(SqlTest, InsertVariants) {
    db->Execute("CREATE TABLE t (a INT, b TEXT, c DOUBLE)");
    db->Execute("INSERT INTO t (b, a) VALUES ('x', 1)");
    auto r = Q("SELECT a, b, c FROM t");
    EXPECT_EQ(r[0][0].AsInt(), 1);
    EXPECT_EQ(r[0][1].AsText(), "x");
    EXPECT_TRUE(r[0][2].IsNull());
    db->Execute("INSERT INTO t VALUES (2, 'y', 3)");  // INT literal into DOUBLE column
    EXPECT_EQ(Q("SELECT c FROM t WHERE a = 2")[0][0].Type(), TypeId::DOUBLE);
    db->Execute("CREATE TABLE copy (a INT, b TEXT, c DOUBLE)");
    EXPECT_EQ(Msg("INSERT INTO copy SELECT * FROM t WHERE a > 0"), "INSERT 2");
    EXPECT_THROW(db->Execute("INSERT INTO t VALUES (1, 'a')"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO t (zz) VALUES (1)"), DbError);
}

TEST_F(SqlTest, Constraints) {
    SeedCompany();
    EXPECT_THROW(db->Execute("INSERT INTO emp VALUES (1, 'dup', 1, 1)"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO emp VALUES (NULL, 'x', 1, 1)"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO emp VALUES ('str', 'x', 1, 1)"), DbError);
    EXPECT_THROW(db->Execute("UPDATE emp SET id = 2 WHERE id = 1"), DbError);
    EXPECT_THROW(db->Execute("UPDATE emp SET id = NULL WHERE id = 1"), DbError);
    EXPECT_NO_THROW(db->Execute("UPDATE emp SET id = 1 WHERE id = 1")) << "same value is not a conflict";
    EXPECT_THROW(db->Execute("CREATE TABLE bad (a INT PRIMARY KEY, b INT PRIMARY KEY)"), DbError);
    EXPECT_THROW(db->Execute("CREATE TABLE bad (a TEXT PRIMARY KEY)"), DbError);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp"), 5) << "failed statements must not change data";
    ExpectIndexesConsistent("emp");
}

// A statement either applies completely or not at all, even when it touches many rows
// and the error is only discovered at the 2nd, 3rd, ... row.
TEST_F(SqlTest, FailedMultiRowInsertChangesNothing) {
    SeedCompany();
    EXPECT_THROW(db->Execute("INSERT INTO emp VALUES (10, 'new', 1, 1), (1, 'dup', 1, 1)"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO emp VALUES (11, 'a', 1, 1), (11, 'b', 1, 1)"), DbError)
        << "duplicates inside one statement";
    EXPECT_THROW(db->Execute("INSERT INTO emp VALUES (12, 'a', 1, 1), (NULL, 'b', 1, 1)"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO emp VALUES (13, 'a', 1, 1), (14, '" + std::string(5000, 'x') + "', 1, 1)"),
                 DbError);
    db->Execute("CREATE TABLE src (id INT)");
    db->Execute("INSERT INTO src VALUES (20), (2)");
    EXPECT_THROW(db->Execute("INSERT INTO emp (id) SELECT id FROM src"), DbError);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp"), 5);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp WHERE id >= 10"), 0);
    ExpectIndexesConsistent("emp");
}

TEST_F(SqlTest, FailedMultiRowUpdateChangesNothing) {
    SeedCompany();
    EXPECT_THROW(db->Execute("UPDATE emp SET salary = 1 / (id - 3)"), DbError) << "fails at the 3rd row";
    EXPECT_THROW(db->Execute("UPDATE emp SET id = 42"), DbError) << "duplicates inside one statement";
    EXPECT_THROW(db->Execute("UPDATE emp SET id = id + 1 WHERE id <> 5"), DbError) << "4 -> 5 is taken";
    db->Execute("UPDATE emp SET id = 15 WHERE id = 5");
    EXPECT_THROW(db->Execute("UPDATE emp SET id = id + 11 WHERE id < 15"), DbError)
        << "1..3 -> 12..14 are free, only the last row (4 -> 15) collides";
    EXPECT_THROW(db->Execute("UPDATE emp SET name = '" + std::string(5000, 'x') + "' WHERE id > 2"), DbError);
    EXPECT_THROW(db->Execute("UPDATE emp SET dept_id = NULL, id = NULL WHERE id > 3"), DbError);

    auto rows = Q("SELECT id, name, salary FROM emp ORDER BY id");
    ASSERT_EQ(rows.size(), 5u);
    const int64_t ids[] = {1, 2, 3, 4, 15};
    const double salaries[] = {100, 80, 50, 70, 60};
    for (size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i][0].AsInt(), ids[i]);
        EXPECT_EQ(rows[i][1].AsText().size(), 3u) << "name of row " << i << " was changed";
        EXPECT_DOUBLE_EQ(rows[i][2].AsDouble(), salaries[i]);
    }
    ExpectIndexesConsistent("emp");
}

// Unique constraints are checked against the state at the end of the statement, so
// shifting all keys at once is fine even though rows temporarily "collide".
TEST_F(SqlTest, UpdateChecksUniquenessAtEndOfStatement) {
    SeedCompany();
    EXPECT_EQ(Msg("UPDATE emp SET id = id + 1"), "UPDATE 5");
    EXPECT_EQ(Scalar("SELECT MIN(id) FROM emp"), 2);
    EXPECT_EQ(Scalar("SELECT MAX(id) FROM emp"), 6);
    EXPECT_EQ(Msg("UPDATE emp SET id = 7 - id"), "UPDATE 5") << "reverses the order: every key swaps";
    EXPECT_EQ(Q("SELECT name FROM emp WHERE id = 1")[0][0].AsText(), "eve");
    EXPECT_EQ(Q("SELECT name FROM emp WHERE id = 5")[0][0].AsText(), "ann");
    ExpectIndexesConsistent("emp");

    // Rows that move to another page (grown tuples) keep their index entries right.
    db->Execute("CREATE INDEX emp_dept ON emp (dept_id)");
    EXPECT_EQ(Msg("UPDATE emp SET name = '" + std::string(1500, 'n') + "', id = id * 2"), "UPDATE 5");
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp WHERE id = 10"), 1);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp WHERE dept_id = 1"), 2);
    ExpectIndexesConsistent("emp");
}

TEST_F(SqlTest, UniqueColumnConstraint) {
    db->Execute("CREATE TABLE u (id INT, code INT UNIQUE)");
    db->Execute("INSERT INTO u VALUES (1, 100), (2, NULL), (3, NULL)");  // NULLs never conflict
    EXPECT_THROW(db->Execute("INSERT INTO u VALUES (4, 100)"), DbError);
}

TEST_F(SqlTest, UpdateAndDelete) {
    SeedCompany();
    EXPECT_EQ(Msg("UPDATE emp SET salary = salary * 2 WHERE dept_id = 2"), "UPDATE 2");
    EXPECT_DOUBLE_EQ(Q("SELECT salary FROM emp WHERE id = 3")[0][0].AsDouble(), 100.0);
    EXPECT_EQ(Msg("UPDATE emp SET name = 'x', dept_id = 3 WHERE id = 5"), "UPDATE 1");
    EXPECT_EQ(Q("SELECT name FROM emp WHERE dept_id = 3")[0][0].AsText(), "x");
    EXPECT_EQ(Msg("DELETE FROM emp WHERE salary < 70"), "DELETE 1");
    EXPECT_EQ(Msg("DELETE FROM emp WHERE id = 999"), "DELETE 0");
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp"), 4);
    EXPECT_EQ(Msg("DELETE FROM emp"), "DELETE 4");
    ExpectIndexesConsistent("emp");
}

TEST_F(SqlTest, UpdateThatMovesTuplesKeepsIndexesValid) {
    db->Execute("CREATE TABLE t (id INT PRIMARY KEY, grp INT, s TEXT); CREATE INDEX t_grp ON t (grp)");
    db->Populate("t", 3000);
    size_t pages_before = db->GetCatalog().RequireTable("t")->heap->PageCount();
    // Grow rows so they no longer fit on their page and must move (their RID changes).
    db->Execute("UPDATE t SET s = '" + std::string(500, 'w') + "' WHERE id <= 1000");
    EXPECT_GT(db->GetCatalog().RequireTable("t")->heap->PageCount(), pages_before);
    ExpectIndexesConsistent("t");
    EXPECT_EQ(Q("SELECT s FROM t WHERE id = 777")[0][0].AsText(), std::string(500, 'w'));
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM t WHERE id <= 1000 AND s LIKE 'w%'"), 1000);
}

TEST_F(SqlTest, IndexMaintenanceUnderChurn) {
    db->Execute("CREATE TABLE t (id INT PRIMARY KEY, grp INT, s TEXT); CREATE INDEX t_grp ON t (grp)");
    db->Populate("t", 5000);
    db->Execute("DELETE FROM t WHERE id % 3 = 0");
    db->Execute("UPDATE t SET grp = grp + 1000 WHERE id % 5 = 0");
    db->Execute("UPDATE t SET id = id + 100000 WHERE id > 4500");
    db->Populate("t", 500);
    ExpectIndexesConsistent("t");
    // index lookups and full scans must agree
    for (int g : {0, 5, 1005, 999}) {
        std::string gs = std::to_string(g);
        EXPECT_EQ(Scalar("SELECT COUNT(*) FROM t WHERE grp = " + gs),
                  Scalar("SELECT COUNT(*) FROM t WHERE grp + 0 = " + gs))
            << "grp = " << g;
    }
}

TEST_F(SqlTest, IndexAndSeqScanAgreeOnRandomRanges) {
    db->Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    db->Populate("t", 10000);
    std::mt19937 rng(99);
    for (int i = 0; i < 25; ++i) {
        int a = static_cast<int>(rng() % 11000) - 500, b = a + static_cast<int>(rng() % 300);
        std::string w = std::to_string(a) + " AND " + std::to_string(b);
        EXPECT_EQ(Scalar("SELECT COUNT(*) FROM t WHERE id BETWEEN " + w),
                  Scalar("SELECT COUNT(*) FROM t WHERE id + 0 BETWEEN " + w))
            << w;
    }
}

TEST_F(SqlTest, HashJoinAndNestedLoopAgree) {
    db->Execute("CREATE TABLE a (id INT PRIMARY KEY, k INT); CREATE TABLE b (id INT PRIMARY KEY, k INT)");
    db->Populate("a", 400);
    db->Populate("b", 300);
    int64_t hash = Scalar("SELECT COUNT(*) FROM a JOIN b ON a.k = b.k");
    int64_t loop = Scalar("SELECT COUNT(*) FROM a JOIN b ON a.k <= b.k AND a.k >= b.k");
    EXPECT_EQ(hash, loop);
    EXPECT_GT(hash, 0);
}

TEST_F(SqlTest, CorrectWithTinyBufferPool) {
    db.reset();
    db = std::make_unique<Database>(file.path(), 8);  // 8 frames = 32 KB of cache
    db->Execute("CREATE TABLE t (id INT PRIMARY KEY, g INT, s TEXT)");
    db->Populate("t", 5000);
    auto r = Q("SELECT a.g, COUNT(*) FROM t a JOIN t b ON a.id = b.id WHERE a.id <= 1000 GROUP BY a.g");
    int64_t total = 0;
    for (auto& row : r) total += row[1].AsInt();
    EXPECT_EQ(total, 1000);
    EXPECT_GT(db->Bpm().Evictions(), 100u);
    EXPECT_EQ(db->Bpm().PinnedFrames(), 0u);
}

TEST_F(SqlTest, LongTextValues) {
    db->Execute("CREATE TABLE docs (id INT PRIMARY KEY, body TEXT)");
    std::string body(3000, 'z');
    db->Execute("INSERT INTO docs VALUES (1, '" + body + "')");
    EXPECT_EQ(Q("SELECT body FROM docs")[0][0].AsText(), body);
    EXPECT_THROW(db->Execute("INSERT INTO docs VALUES (2, '" + std::string(5000, 'x') + "')"), DbError);
}

TEST_F(SqlTest, ExplainAndExplainAnalyze) {
    SeedCompany();
    auto plan = Q("EXPLAIN SELECT * FROM emp WHERE id = 1");
    ASSERT_FALSE(plan.empty());
    EXPECT_NE(plan[0][0].AsText().find("Projection"), std::string::npos);
    auto analyzed = Q("EXPLAIN ANALYZE SELECT * FROM emp");
    EXPECT_NE(analyzed[0][0].AsText().find("actual rows=5"), std::string::npos) << analyzed[0][0].AsText();
    EXPECT_NE(analyzed.back()[0].AsText().find("Execution Time"), std::string::npos);
}

TEST_F(SqlTest, IntegerOverflowNeverReachesTheTable) {
    db->Execute("CREATE TABLE n (id INT PRIMARY KEY, v INT)");
    db->Execute("INSERT INTO n VALUES (1, 9223372036854775807), (2, -9223372036854775808), (3, 0)");
    EXPECT_THROW(db->Execute("UPDATE n SET v = v + 1"), DbError);
    EXPECT_THROW(db->Execute("UPDATE n SET v = -v WHERE id = 2"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO n VALUES (4, 1e19)"), DbError);
    EXPECT_EQ(Q("SELECT SUM(v) FROM n WHERE id <> 2")[0][0].AsInt(), INT64_MAX) << "MAX + 0 fits";
    EXPECT_THROW(db->Execute("SELECT SUM(n.v) FROM n, n m WHERE n.id = 1"), DbError) << "3 * MAX does not";
    EXPECT_THROW(db->Execute("SELECT v / -1 FROM n WHERE id = 2"), DbError) << "used to crash the process";
    EXPECT_EQ(Q("SELECT v % -1 FROM n WHERE id = 2")[0][0].AsInt(), 0);
    EXPECT_EQ(Q("SELECT v FROM n WHERE id = 1")[0][0].AsInt(), INT64_MAX);
    EXPECT_EQ(Q("SELECT v FROM n WHERE id = 2")[0][0].AsInt(), INT64_MIN);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM n WHERE v = -9223372036854775808"), 1);
}

TEST_F(SqlTest, NonBooleanConditionsAreErrors) {
    SeedCompany();
    EXPECT_THROW(db->Execute("SELECT * FROM emp WHERE 1"), DbError);
    EXPECT_THROW(db->Execute("SELECT * FROM emp WHERE name"), DbError);
    EXPECT_THROW(db->Execute("SELECT * FROM emp WHERE id = 1 AND salary"), DbError)
        << "AND checks its operands";
    EXPECT_THROW(db->Execute("SELECT * FROM emp e JOIN dept d ON e.dept_id"), DbError);
    EXPECT_THROW(db->Execute("SELECT dept_id FROM emp GROUP BY dept_id HAVING COUNT(*)"), DbError);
    EXPECT_THROW(db->Execute("DELETE FROM emp WHERE id + 1"), DbError);
    EXPECT_THROW(db->Execute("UPDATE emp SET name = 'x' WHERE salary * 2"), DbError);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp WHERE dept_id = NULL"), 0) << "NULL is not an error";
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp WHERE TRUE"), 5);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp"), 5) << "the failed DELETE removed nothing";
}

TEST_F(SqlTest, ColumnsMayBeTargetedOnlyOnce) {
    db->Execute("CREATE TABLE t (a INT, b INT)");
    EXPECT_THROW(db->Execute("INSERT INTO t (a, a) VALUES (1, 2)"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO t (a, b, A) VALUES (1, 2, 3)"), DbError) << "identifiers are case-insensitive";
    db->Execute("INSERT INTO t (b, a) VALUES (2, 1)");
    EXPECT_THROW(db->Execute("UPDATE t SET a = 5, a = 6"), DbError);
    EXPECT_THROW(db->Execute("UPDATE t SET a = 5, b = 1, a = 6"), DbError);
    auto rows = Q("SELECT a, b FROM t");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].AsInt(), 1);
    EXPECT_EQ(rows[0][1].AsInt(), 2);
}

TEST_F(SqlTest, ErrorsDoNotBreakTheSession) {
    SeedCompany();
    EXPECT_THROW(db->Execute("SELEC 1"), DbError);
    EXPECT_THROW(db->Execute("SELECT nope FROM emp"), DbError);
    EXPECT_THROW(db->Execute("SELECT 1 / 0"), DbError);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp"), 5);
}

}  // namespace
}  // namespace minidb
