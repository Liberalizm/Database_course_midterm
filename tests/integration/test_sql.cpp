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
            int64_t non_null = Scalar("SELECT COUNT(" + idx->columns[0] + ") FROM " + table);
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
    EXPECT_THROW(db->Execute("CREATE TABLE bad (a DOUBLE PRIMARY KEY)"), DbError);
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

TEST_F(SqlTest, LeftJoin) {
    SeedCompany();
    auto r = Q("SELECT d.name, e.name FROM dept d LEFT JOIN emp e ON e.dept_id = d.id ORDER BY d.id, e.id");
    ASSERT_EQ(r.size(), 5u);
    EXPECT_EQ(r[4][0].AsText(), "empty");
    EXPECT_TRUE(r[4][1].IsNull()) << "a department without employees is kept";
    auto counts = Q("SELECT d.name, COUNT(e.id) FROM dept d LEFT JOIN emp e ON e.dept_id = d.id "
                    "GROUP BY d.name ORDER BY d.name");
    ASSERT_EQ(counts.size(), 3u);
    EXPECT_EQ(counts[0][1].AsInt(), 0);  // empty
    EXPECT_EQ(counts[1][1].AsInt(), 2);  // eng
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp e LEFT JOIN dept d ON e.dept_id = d.id WHERE d.id IS NULL"), 1)
        << "anti-join: eve has no department";
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp e LEFT JOIN dept d ON e.dept_id = d.id LEFT JOIN emp m ON "
                     "m.dept_id = d.id AND m.salary > e.salary"),
              5)
        << "chained left joins: bob -> ann and cid -> dan earn more; ann, dan, eve get one padded row each";
    // Usable as a source for INSERT ... SELECT.
    db->Execute("CREATE TABLE report (dept TEXT, n INT)");
    EXPECT_EQ(Msg("INSERT INTO report SELECT d.name, e.id FROM dept d LEFT JOIN emp e ON e.dept_id = d.id"),
              "INSERT 5");
}

TEST_F(SqlTest, FunctionsCaseConcatAndDistinctAggregates) {
    SeedCompany();
    auto r = Q("SELECT UPPER(name) || ' (' || COALESCE(dept_id, 0) || ')' FROM emp ORDER BY id");
    ASSERT_EQ(r.size(), 5u);
    EXPECT_EQ(r[0][0].AsText(), "ANN (1)");
    EXPECT_EQ(r[4][0].AsText(), "EVE (0)");
    auto bands = Q("SELECT CASE WHEN salary >= 80 THEN 'high' WHEN salary >= 60 THEN 'mid' ELSE 'low' END AS band, "
                   "COUNT(*) FROM emp GROUP BY CASE WHEN salary >= 80 THEN 'high' WHEN salary >= 60 THEN 'mid' "
                   "ELSE 'low' END ORDER BY band");
    ASSERT_EQ(bands.size(), 3u);
    EXPECT_EQ(bands[0][0].AsText(), "high");
    EXPECT_EQ(bands[0][1].AsInt(), 2);
    EXPECT_EQ(bands[1][0].AsText(), "low");
    EXPECT_EQ(bands[2][1].AsInt(), 2);
    EXPECT_EQ(Scalar("SELECT COUNT(DISTINCT dept_id) FROM emp"), 2);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp WHERE LENGTH(name) = 3 AND LOWER(SUBSTR(name, 1, 1)) = 'a'"), 1);
    EXPECT_EQ(Scalar("SELECT SUM(CASE WHEN dept_id IS NULL THEN 1 ELSE 0 END) FROM emp"), 1);
    // Functions in DML and in joins.
    EXPECT_EQ(Msg("UPDATE emp SET name = UPPER(SUBSTR(name, 1, 1)) || SUBSTR(name, 2) WHERE dept_id = 1"), "UPDATE 2");
    EXPECT_EQ(Q("SELECT name FROM emp WHERE id = 1")[0][0].AsText(), "Ann");
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp e LEFT JOIN dept d ON e.dept_id = d.id WHERE COALESCE(d.name, '-') = '-'"),
              1);
    EXPECT_THROW(db->Execute("SELECT NOSUCHFN(id) FROM emp"), DbError);
    EXPECT_THROW(db->Execute("SELECT name FROM emp GROUP BY UPPER(name)"), DbError)
        << "name itself is not grouped, only UPPER(name)";
}

TEST_F(SqlTest, TextPrimaryKeyAndUniqueColumns) {
    db->Execute("CREATE TABLE users (name TEXT PRIMARY KEY, email TEXT UNIQUE, age INT)");
    db->Execute("INSERT INTO users VALUES ('ann', 'ann@x', 30), ('bob', NULL, 25), ('cid', NULL, 41)");
    EXPECT_THROW(db->Execute("INSERT INTO users VALUES ('ann', 'other@x', 1)"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO users VALUES ('dan', 'ann@x', 1)"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO users VALUES ('eve', 'e@x', 1), ('eve', 'f@x', 2)"), DbError)
        << "duplicates inside one statement";
    EXPECT_THROW(db->Execute("INSERT INTO users VALUES (NULL, 'n@x', 1)"), DbError) << "PRIMARY KEY is NOT NULL";
    EXPECT_NO_THROW(db->Execute("INSERT INTO users VALUES ('Ann', 'ANN@x', 1)")) << "case-sensitive like PostgreSQL";
    EXPECT_THROW(db->Execute("UPDATE users SET email = 'ann@x' WHERE name = 'bob'"), DbError);
    // Swapping two keys in one statement is fine: uniqueness holds at the end.
    EXPECT_EQ(Msg("UPDATE users SET name = CASE name WHEN 'bob' THEN 'cid' ELSE 'bob' END "
                  "WHERE name = 'bob' OR name = 'cid'"),
              "UPDATE 2");
    EXPECT_EQ(Scalar("SELECT age FROM users WHERE name = 'cid'"), 25);
    EXPECT_EQ(Msg("DELETE FROM users WHERE name = 'Ann'"), "DELETE 1");
    EXPECT_NO_THROW(db->Execute("INSERT INTO users VALUES ('Ann', 'ANN@x', 1)")) << "the deleted key is free again";
    ExpectIndexesConsistent("users");
}

TEST_F(SqlTest, TextIndexIsUsedForEqualityOnly) {
    db->Execute("CREATE TABLE t (id INT PRIMARY KEY, code TEXT)");
    db->Populate("t", 3000);
    db->Execute("CREATE INDEX t_code ON t (code)");
    std::string code = Q("SELECT code FROM t WHERE id = 1234")[0][0].AsText();
    auto plan = Q("EXPLAIN SELECT id FROM t WHERE code = '" + code + "'");
    std::string text;
    for (auto& r : plan) text += r[0].AsText() + "\n";
    EXPECT_NE(text.find("Index Scan using t_code"), std::string::npos) << text;
    EXPECT_NE(text.find("hash(t.code)"), std::string::npos) << text;
    EXPECT_NE(text.find("Filter: t.code = '" + code + "'"), std::string::npos) << "re-checked because of collisions\n"
                                                                             << text;
    auto range = Q("EXPLAIN SELECT id FROM t WHERE code > 'm'");
    EXPECT_EQ(range[1][0].AsText().find("Index Scan"), std::string::npos) << "hashes cannot answer ranges";
    // Same answers with and without the index.
    for (const char* c : {"alpha_1", "kilo_500", "nope", "papa_999"}) {
        std::string q = std::string("SELECT COUNT(*) FROM t WHERE code = '") + c + "'";
        int64_t with_index = Scalar(q);
        int64_t by_scan = Scalar(std::string("SELECT COUNT(*) FROM t WHERE code || '' = '") + c + "'");
        EXPECT_EQ(with_index, by_scan) << c;
    }
    ExpectIndexesConsistent("t");
}

TEST_F(SqlTest, PopulateFillsUniqueTextColumns) {
    db->Execute("CREATE TABLE u (id INT PRIMARY KEY, login TEXT UNIQUE)");
    db->Execute("INSERT INTO u VALUES (1, 'login_3')");  // a value Populate would also produce
    db->Populate("u", 500);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM u"), 501);
    EXPECT_EQ(Scalar("SELECT COUNT(DISTINCT login) FROM u"), 501);
    ExpectIndexesConsistent("u");
}

TEST_F(SqlTest, UncorrelatedSubqueries) {
    SeedCompany();
    // Scalar subquery: above-average earners (avg = 72).
    auto above = Q("SELECT name FROM emp WHERE salary > (SELECT AVG(salary) FROM emp) ORDER BY name");
    ASSERT_EQ(above.size(), 2u);
    EXPECT_EQ(above[0][0].AsText(), "ann");
    EXPECT_EQ(Scalar("SELECT (SELECT MAX(id) FROM emp) + 1"), 6);
    EXPECT_TRUE(Q("SELECT (SELECT name FROM emp WHERE id = 99)")[0][0].IsNull()) << "no row -> NULL";
    // IN / NOT IN, including the NULL trap (eve's dept_id is NULL).
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM dept WHERE id IN (SELECT dept_id FROM emp)"), 2);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM dept WHERE id NOT IN (SELECT dept_id FROM emp)"), 0)
        << "NOT IN against a set containing NULL is never true";
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM dept WHERE id NOT IN (SELECT dept_id FROM emp WHERE dept_id IS NOT NULL)"), 1);
    // EXISTS, nesting, use in joins and aggregates.
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM dept WHERE EXISTS (SELECT 1 FROM emp WHERE salary > 90)"), 3);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM dept WHERE NOT EXISTS (SELECT 1 FROM emp WHERE salary > 900)"), 3);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp WHERE dept_id IN "
                     "(SELECT id FROM dept WHERE name IN (SELECT 'eng'))"),
              2);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp e JOIN dept d ON e.dept_id = d.id "
                     "WHERE d.id = (SELECT MIN(id) FROM dept)"),
              2);
    // Errors.
    EXPECT_THROW(db->Execute("SELECT (SELECT name FROM emp)"), DbError) << "more than one row";
    EXPECT_THROW(db->Execute("SELECT * FROM emp WHERE id IN (SELECT id, name FROM emp)"), DbError);
    EXPECT_EQ(Q("SELECT name FROM emp e WHERE salary = (SELECT MAX(salary) FROM emp x "
                "WHERE x.dept_id = e.dept_id)").size(),
              2u)
        << "correlated subqueries work now (see CorrelatedSubqueries): ann and dan";
}

TEST_F(SqlTest, CorrelatedSubqueries) {
    SeedCompany();  // eng: ann 100, bob 80 | sales: cid 50, dan 70 | eve: no dept, 60 | dept 3 "empty"
    auto names = [&](const std::string& sql) {
        std::vector<std::string> out;
        for (auto& r : Q(sql)) out.push_back(r[0].AsText());
        return out;
    };
    using V = std::vector<std::string>;
    EXPECT_EQ(names("SELECT d.name FROM dept d WHERE EXISTS (SELECT 1 FROM emp e WHERE e.dept_id = d.id) "
                    "ORDER BY d.name"),
              (V{"eng", "sales"}));
    EXPECT_EQ(names("SELECT d.name FROM dept d WHERE NOT EXISTS (SELECT 1 FROM emp e WHERE e.dept_id = d.id)"),
              (V{"empty"}));
    // Scalar: above their own department's average (eve's NULL department matches nothing).
    EXPECT_EQ(names("SELECT e.name FROM emp e WHERE e.salary > "
                    "(SELECT AVG(x.salary) FROM emp x WHERE x.dept_id = e.dept_id) ORDER BY e.name"),
              (V{"ann", "dan"}));
    // In the SELECT list, per row.
    auto staff = Q("SELECT d.name, (SELECT COUNT(*) FROM emp e WHERE e.dept_id = d.id) FROM dept d ORDER BY d.id");
    ASSERT_EQ(staff.size(), 3u);
    EXPECT_EQ(staff[0][1].AsInt(), 2);
    EXPECT_EQ(staff[2][1].AsInt(), 0);
    // IN, unqualified outer reference, and two levels of nesting.
    EXPECT_EQ(names("SELECT name FROM dept WHERE 'bob' IN (SELECT name FROM emp WHERE dept_id = dept.id)"),
              (V{"eng"}));
    EXPECT_EQ(names("SELECT d.name FROM dept d WHERE EXISTS (SELECT 1 FROM emp e WHERE e.dept_id = d.id AND "
                    "EXISTS (SELECT 1 FROM emp f WHERE f.dept_id = d.id AND f.salary > e.salary)) ORDER BY 1"),
              (V{"eng", "sales"}));
    // With joins, GROUP BY and in DML.
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp e JOIN dept d ON e.dept_id = d.id WHERE e.salary = "
                     "(SELECT MIN(salary) FROM emp WHERE dept_id = d.id)"),
              2);
    EXPECT_EQ(Msg("DELETE FROM emp WHERE NOT EXISTS (SELECT 1 FROM dept d WHERE d.id = emp.dept_id)"), "DELETE 1");
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp"), 4);
    // Errors are still reported.
    EXPECT_THROW(db->Execute("SELECT name FROM emp e WHERE salary = (SELECT x.salary FROM emp x "
                             "WHERE x.dept_id = e.dept_id)"),
                 DbError)
        << "more than one row for eng";
    EXPECT_THROW(db->Execute("SELECT name FROM emp WHERE EXISTS (SELECT 1 FROM dept WHERE dept.id = emp.nope)"),
                 DbError);
}

// Differential: correlated EXISTS / IN must agree with the equivalent joins.
TEST_F(SqlTest, CorrelatedSubqueriesAgreeWithJoinsOnRandomData) {
    db->Execute("CREATE TABLE a (id INT PRIMARY KEY, k INT); CREATE TABLE b (id INT PRIMARY KEY, k INT)");
    db->Populate("a", 300);
    db->Populate("b", 200);
    db->Execute("UPDATE a SET k = NULL WHERE id % 13 = 0");
    db->Execute("UPDATE b SET k = k + 500 WHERE id % 2 = 0");
    int64_t with_partner = Scalar("SELECT COUNT(*) FROM (SELECT DISTINCT a.id FROM a JOIN b ON a.k = b.k) x");
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.k = a.k)"), with_partner);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM a WHERE a.k IN (SELECT b.k FROM b WHERE b.k = a.k)"), with_partner);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM a WHERE NOT EXISTS (SELECT 1 FROM b WHERE b.k = a.k)"), 300 - with_partner);
    EXPECT_EQ(Scalar("SELECT SUM((SELECT COUNT(*) FROM b WHERE b.k = a.k)) FROM a"),
              Scalar("SELECT COUNT(*) FROM a JOIN b ON a.k = b.k"));
}

TEST_F(SqlTest, SubqueriesInDml) {
    SeedCompany();
    // The subquery sees the table as it was before the statement.
    EXPECT_EQ(Msg("DELETE FROM emp WHERE id IN (SELECT id FROM emp WHERE salary < 60)"), "DELETE 1");
    EXPECT_EQ(Msg("UPDATE emp SET salary = (SELECT MAX(salary) FROM emp) WHERE name = 'eve'"), "UPDATE 1");
    EXPECT_EQ(Scalar("SELECT salary FROM emp WHERE name = 'eve'"), 100);
    EXPECT_EQ(Msg("INSERT INTO dept VALUES ((SELECT MAX(id) + 1 FROM dept), 'new')"), "INSERT 1");
    EXPECT_EQ(Q("SELECT name FROM dept WHERE id = 4")[0][0].AsText(), "new");
    EXPECT_EQ(Msg("INSERT INTO dept SELECT id + 10, name FROM dept WHERE id IN (SELECT dept_id FROM emp)"),
              "INSERT 2");
    ExpectIndexesConsistent("emp");
    ExpectIndexesConsistent("dept");
}

TEST_F(SqlTest, UnionIntersectExcept) {
    SeedCompany();
    // Departments that have employees, and those that don't.
    auto staffed = Q("SELECT id FROM dept INTERSECT SELECT dept_id FROM emp ORDER BY id");
    ASSERT_EQ(staffed.size(), 2u);
    EXPECT_EQ(staffed[0][0].AsInt(), 1);
    auto empty = Q("SELECT id FROM dept EXCEPT SELECT dept_id FROM emp");
    ASSERT_EQ(empty.size(), 1u);
    EXPECT_EQ(empty[0][0].AsInt(), 3);
    // UNION removes duplicates, UNION ALL keeps them; names come from the first query.
    auto names = db->Execute("SELECT name AS who FROM dept UNION SELECT name FROM emp ORDER BY who DESC LIMIT 2").back();
    ASSERT_EQ(names.columns.at(0), "who");
    ASSERT_EQ(names.rows.size(), 2u);
    EXPECT_EQ(names.rows[0][0].AsText(), "sales");
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp WHERE dept_id IN (SELECT dept_id FROM emp UNION SELECT 3)"), 4);
    EXPECT_EQ(Q("SELECT dept_id FROM emp UNION ALL SELECT dept_id FROM emp").size(), 10u);
    EXPECT_EQ(Q("SELECT dept_id FROM emp UNION SELECT dept_id FROM emp").size(), 3u) << "1, 2, NULL";
    // As the source of INSERT ... SELECT.
    db->Execute("CREATE TABLE ids (v INT)");
    EXPECT_EQ(Msg("INSERT INTO ids SELECT id FROM emp UNION SELECT id FROM dept"), "INSERT 5");
    // Errors.
    EXPECT_THROW(db->Execute("SELECT id, name FROM emp UNION SELECT id FROM dept"), DbError);
    EXPECT_THROW(db->Execute("SELECT id FROM emp UNION SELECT id FROM dept ORDER BY emp.id"), DbError);
    EXPECT_THROW(db->Execute("SELECT id FROM emp UNION SELECT id FROM dept ORDER BY 2"), DbError);
}

TEST_F(SqlTest, SubqueriesInFrom) {
    SeedCompany();
    // Filter on an aggregate through a derived table.
    auto big = Q("SELECT g.dept_id, g.n FROM (SELECT dept_id, COUNT(*) AS n FROM emp GROUP BY dept_id) g "
                 "WHERE g.n > 1 ORDER BY g.dept_id");
    ASSERT_EQ(big.size(), 2u);
    EXPECT_EQ(big[0][1].AsInt(), 2);
    // Aggregate over an aggregate.
    EXPECT_EQ(Scalar("SELECT MAX(n) FROM (SELECT dept_id, COUNT(*) AS n FROM emp GROUP BY dept_id) g"), 2);
    // Joins, including LEFT JOIN against a derived table.
    auto totals = Q("SELECT d.name, s.total FROM dept d LEFT JOIN "
                    "(SELECT dept_id, SUM(salary) AS total FROM emp GROUP BY dept_id) s ON s.dept_id = d.id "
                    "ORDER BY d.id");
    ASSERT_EQ(totals.size(), 3u);
    EXPECT_DOUBLE_EQ(totals[0][1].AsDouble(), 180.0);
    EXPECT_TRUE(totals[2][1].IsNull()) << "the empty department";
    // SELECT * uses the subquery's column names; nesting and set operations work.
    auto star = db->Execute("SELECT * FROM (SELECT name AS who, salary FROM emp WHERE id = 1) t").back();
    ASSERT_EQ(star.columns.size(), 2u);
    EXPECT_EQ(star.columns[0], "who");
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM (SELECT * FROM (SELECT id FROM emp UNION SELECT id FROM dept) u "
                     "WHERE u.id > 2) v"),
              3);
    // Inside DML subqueries.
    EXPECT_EQ(Msg("DELETE FROM emp WHERE id IN (SELECT s.id FROM (SELECT id, salary FROM emp) s WHERE s.salary < 60)"),
              "DELETE 1");
    // Errors.
    EXPECT_THROW(db->Execute("SELECT * FROM (SELECT e.id, d.id FROM emp e JOIN dept d ON e.dept_id = d.id) z"), DbError)
        << "duplicate column names";
    EXPECT_THROW(db->Execute("SELECT * FROM (SELECT id FROM emp) a, (SELECT id FROM dept) a"), DbError);
    EXPECT_THROW(db->Execute("SELECT nope FROM (SELECT id FROM emp) a"), DbError);
}

TEST_F(SqlTest, AlterTable) {
    SeedCompany();
    db->Execute("CREATE INDEX emp_dept ON emp (dept_id)");
    // ADD COLUMN: existing rows get NULL; UNIQUE creates an index.
    EXPECT_EQ(Msg("ALTER TABLE emp ADD COLUMN email TEXT UNIQUE"), "ALTER TABLE");
    EXPECT_EQ(Scalar("SELECT COUNT(email) FROM emp"), 0);
    db->Execute("UPDATE emp SET email = name || '@corp' WHERE id <= 2");
    EXPECT_THROW(db->Execute("UPDATE emp SET email = 'ann@corp' WHERE id = 3"), DbError);
    EXPECT_THROW(db->Execute("ALTER TABLE emp ADD COLUMN level INT NOT NULL"), DbError) << "table has rows";
    EXPECT_THROW(db->Execute("ALTER TABLE emp ADD COLUMN name TEXT"), DbError) << "exists already";
    // DROP COLUMN removes the data and the indexes on it; other indexes keep working.
    EXPECT_EQ(Msg("ALTER TABLE emp DROP COLUMN dept_id"), "ALTER TABLE");
    EXPECT_THROW(db->Execute("SELECT dept_id FROM emp"), DbError);
    for (IndexInfo* idx : db->GetCatalog().IndexesOf("emp")) EXPECT_EQ(std::count(idx->columns.begin(), idx->columns.end(), "dept_id"), 0);
    EXPECT_EQ(Q("SELECT name FROM emp WHERE id = 4")[0][0].AsText(), "dan");
    EXPECT_THROW(db->Execute("INSERT INTO emp (id, name) VALUES (1, 'dup')"), DbError) << "primary key still enforced";
    // RENAME COLUMN / TABLE.
    db->Execute("ALTER TABLE emp RENAME COLUMN salary TO pay");
    EXPECT_DOUBLE_EQ(Q("SELECT pay FROM emp WHERE id = 1")[0][0].AsDouble(), 100.0);
    db->Execute("ALTER TABLE emp RENAME TO staff");
    EXPECT_THROW(db->Execute("SELECT * FROM emp"), DbError);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM staff WHERE email IS NOT NULL"), 2);
    ExpectIndexesConsistent("staff");
    // Survives a restart.
    db.reset();
    db = std::make_unique<Database>(file.path(), 64);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM staff"), 5);
    EXPECT_THROW(db->Execute("INSERT INTO staff (id, name, email) VALUES (9, 'x', 'ann@corp')"), DbError);
    ExpectIndexesConsistent("staff");
}

TEST_F(SqlTest, AlterTableOnEmptyTableAllowsNotNullAndPrimaryKey) {
    db->Execute("CREATE TABLE t (a INT)");
    EXPECT_EQ(Msg("ALTER TABLE t ADD id INT PRIMARY KEY"), "ALTER TABLE");
    EXPECT_THROW(db->Execute("INSERT INTO t (a) VALUES (1)"), DbError) << "id is NOT NULL now";
    db->Execute("INSERT INTO t VALUES (1, 10)");
    EXPECT_THROW(db->Execute("INSERT INTO t VALUES (2, 10)"), DbError) << "and the primary key";
    EXPECT_THROW(db->Execute("ALTER TABLE t ADD other INT PRIMARY KEY"), DbError);
    EXPECT_EQ(Msg("ALTER TABLE t DROP COLUMN a"), "ALTER TABLE");
    EXPECT_THROW(db->Execute("ALTER TABLE t DROP COLUMN id"), DbError) << "the only column left";
    EXPECT_EQ(Scalar("SELECT id FROM t"), 10) << "id was added after a: VALUES (1, 10) set a = 1, id = 10";
}

TEST_F(SqlTest, ColumnDefaults) {
    db->Execute("CREATE TABLE orders (id INT PRIMARY KEY, status TEXT NOT NULL DEFAULT 'new', qty INT DEFAULT 2 * 5, "
                "paid BOOL DEFAULT FALSE, note TEXT)");
    db->Execute("INSERT INTO orders (id) VALUES (1)");
    auto r = Q("SELECT status, qty, paid, note FROM orders WHERE id = 1");
    EXPECT_EQ(r[0][0].AsText(), "new");
    EXPECT_EQ(r[0][1].AsInt(), 10) << "a constant expression, evaluated once";
    EXPECT_FALSE(r[0][2].AsBool());
    EXPECT_TRUE(r[0][3].IsNull()) << "no DEFAULT -> NULL";
    db->Execute("INSERT INTO orders (id, qty) VALUES (2, NULL)");
    EXPECT_TRUE(Q("SELECT qty FROM orders WHERE id = 2")[0][0].IsNull()) << "an explicit NULL is not replaced";
    EXPECT_THROW(db->Execute("INSERT INTO orders (id, status) VALUES (3, NULL)"), DbError) << "NOT NULL still applies";
    db->Execute("CREATE TABLE src (k INT)");
    db->Execute("INSERT INTO src VALUES (7), (8)");
    EXPECT_EQ(Msg("INSERT INTO orders (id) SELECT k FROM src"), "INSERT 2");
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM orders WHERE status = 'new'"), 4) << "ids 1, 2, 7, 8";

    // ALTER TABLE ADD with a DEFAULT fills the existing rows, so NOT NULL works on a non-empty table.
    EXPECT_EQ(Msg("ALTER TABLE orders ADD COLUMN priority INT NOT NULL DEFAULT 3"), "ALTER TABLE");
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM orders WHERE priority = 3"), 4);
    EXPECT_THROW(db->Execute("ALTER TABLE orders ADD COLUMN code INT UNIQUE DEFAULT 1"), DbError)
        << "the same default in every row would violate UNIQUE";
    EXPECT_EQ(Msg("ALTER TABLE orders ADD COLUMN code INT UNIQUE"), "ALTER TABLE") << "NULLs do not conflict";

    // Invalid defaults.
    EXPECT_THROW(db->Execute("CREATE TABLE bad (a INT DEFAULT b, b INT)"), DbError) << "not a constant";
    EXPECT_THROW(db->Execute("CREATE TABLE bad (a INT NOT NULL DEFAULT NULL)"), DbError);
    EXPECT_THROW(db->Execute("CREATE TABLE bad (a INT DEFAULT 'text')"), DbError);
    EXPECT_THROW(db->Execute("CREATE TABLE bad (a INT DEFAULT 1 / 0)"), DbError);
    EXPECT_EQ(db->GetCatalog().GetTable("bad"), nullptr);

    // Defaults survive a restart.
    db.reset();
    db = std::make_unique<Database>(file.path(), 64);
    db->Execute("INSERT INTO orders (id) VALUES (99)");
    EXPECT_EQ(Q("SELECT status FROM orders WHERE id = 99")[0][0].AsText(), "new");
    EXPECT_EQ(Scalar("SELECT priority FROM orders WHERE id = 99"), 3);
}

TEST_F(SqlTest, QuotedIdentifiersAreNeverKeywords) {
    db->Execute("CREATE TABLE \"select\" (\"left\" INT, \"right\" TEXT, \"null\" INT)");
    db->Execute("INSERT INTO \"select\" (\"left\", \"right\", \"null\") VALUES (1, 'r', 7)");
    auto r = Q("SELECT \"left\", \"right\", \"null\" FROM \"select\" WHERE \"left\" = 1");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0][1].AsText(), "r");
    EXPECT_EQ(r[0][2].AsInt(), 7) << "\"null\" is a column, not the NULL literal";
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM \"select\" \"left\" WHERE \"left\".\"left\" = 1"), 1) << "as an alias too";
}

// Differential: LEFT JOIN = inner join + every unmatched left row padded with NULLs,
// for both the hash and the nested loop algorithm.
TEST_F(SqlTest, LeftJoinMatchesItsDefinitionOnRandomData) {
    db->Execute("CREATE TABLE a (id INT PRIMARY KEY, k INT); CREATE TABLE b (id INT PRIMARY KEY, k INT)");
    db->Populate("a", 300);
    db->Populate("b", 120);
    db->Execute("UPDATE a SET k = NULL WHERE id % 17 = 0");
    db->Execute("UPDATE b SET k = k + 1000 WHERE id % 3 = 0");  // fewer partners
    int64_t inner = Scalar("SELECT COUNT(*) FROM a JOIN b ON a.k = b.k");
    int64_t unmatched = 0;
    std::map<int64_t, int> bkeys;
    for (auto& row : Q("SELECT k FROM b")) bkeys[row[0].AsInt()]++;
    for (auto& row : Q("SELECT k FROM a"))
        if (row[0].IsNull() || !bkeys.count(row[0].AsInt())) ++unmatched;
    ASSERT_GT(unmatched, 0);
    int64_t hash = Scalar("SELECT COUNT(*) FROM a LEFT JOIN b ON a.k = b.k");
    int64_t loop = Scalar("SELECT COUNT(*) FROM a LEFT JOIN b ON a.k <= b.k AND a.k >= b.k");
    EXPECT_EQ(hash, inner + unmatched);
    EXPECT_EQ(loop, inner + unmatched);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM a LEFT JOIN b ON a.k = b.k WHERE b.id IS NULL"), unmatched);
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

// ---- multi-column indexes, composite PRIMARY KEY / UNIQUE ----

std::string ErrorOf(Database& db, const std::string& sql) {
    try {
        db.Execute(sql);
    } catch (const DbError& e) {
        return e.what();
    }
    return "(no error)";
}

TEST_F(SqlTest, CompositePrimaryKeyIsEnforced) {
    db->Execute("CREATE TABLE ol (order_id INT, line INT, item TEXT, PRIMARY KEY (order_id, line))");
    db->Execute("INSERT INTO ol VALUES (1, 1, 'a'), (1, 2, 'b'), (2, 1, 'c')");
    ASSERT_EQ(db->GetCatalog().IndexesOf("ol").size(), 1u);
    EXPECT_EQ(db->GetCatalog().IndexesOf("ol")[0]->name, "ol_pkey");

    EXPECT_EQ(ErrorOf(*db, "INSERT INTO ol VALUES (1, 1, 'dup')"),
              "duplicate key value violates unique constraint \"ol_pkey\": (order_id, line)=(1, 1)");
    EXPECT_EQ(Msg("INSERT INTO ol VALUES (2, 2, 'same order, new line'), (3, 1, 'new order')"), "INSERT 2");
    EXPECT_THROW(db->Execute("INSERT INTO ol VALUES (1, NULL, 'x')"), DbError) << "key columns are NOT NULL";
    EXPECT_THROW(db->Execute("INSERT INTO ol (line, item) VALUES (9, 'x')"), DbError);
    EXPECT_THROW(db->Execute("INSERT INTO ol VALUES (4, 1, 'x'), (4, 1, 'y')"), DbError) << "within one statement";
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM ol"), 5) << "failed statements change nothing";

    // Uniqueness holds for the end state of an UPDATE: swapping two keys is fine...
    EXPECT_EQ(Msg("UPDATE ol SET line = 3 - line WHERE order_id = 1"), "UPDATE 2");
    EXPECT_EQ(Q("SELECT item FROM ol WHERE order_id = 1 AND line = 1")[0][0].AsText(), "b");
    // ...colliding with a row that is not updated is not.
    EXPECT_EQ(ErrorOf(*db, "UPDATE ol SET order_id = 1 WHERE order_id = 2 AND line = 1"),
              "duplicate key value violates unique constraint \"ol_pkey\": (order_id, line)=(1, 1)");
    EXPECT_THROW(db->Execute("UPDATE ol SET line = 1 WHERE order_id = 2"), DbError) << "two targets collide";
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM ol WHERE order_id = 2"), 2);
    EXPECT_EQ(Msg("UPDATE ol SET item = 'changed' WHERE order_id = 2"), "UPDATE 2") << "keys unchanged: fine";
    EXPECT_EQ(Msg("DELETE FROM ol WHERE order_id = 1 AND line = 1"), "DELETE 1");
    EXPECT_EQ(Msg("INSERT INTO ol VALUES (1, 1, 'again')"), "INSERT 1") << "a deleted key is free again";
    ExpectIndexesConsistent("ol");
}

TEST_F(SqlTest, InvalidCompositeKeysCreateNothing) {
    EXPECT_EQ(ErrorOf(*db, "CREATE TABLE x (a INT PRIMARY KEY, b INT, PRIMARY KEY (a, b))"),
              "multiple primary keys for table \"x\" are not allowed");
    EXPECT_THROW(db->Execute("CREATE TABLE x (a INT, d DOUBLE, PRIMARY KEY (a, d))"), DbError) << "DOUBLE key";
    EXPECT_THROW(db->Execute("CREATE TABLE x (a INT, b INT, UNIQUE (b, a, b))"), DbError);
    EXPECT_EQ(db->GetCatalog().GetTable("x"), nullptr);
    // A constraint whose index cannot be created (the name is taken) leaves no table behind.
    db->Execute("CREATE TABLE other (v INT); CREATE INDEX y_pkey ON other (v)");
    EXPECT_THROW(db->Execute("CREATE TABLE y (a INT, b INT, PRIMARY KEY (a, b))"), DbError);
    EXPECT_EQ(db->GetCatalog().GetTable("y"), nullptr);
    EXPECT_EQ(Msg("CREATE TABLE y (a INT, b INT)"), "CREATE TABLE");
}

TEST_F(SqlTest, CompositeUniqueConstraintTreatsNullsAsDistinct) {
    db->Execute("CREATE TABLE u (a INT, b TEXT, c INT UNIQUE, UNIQUE (a, b), UNIQUE (c))");
    std::vector<std::string> names;
    for (IndexInfo* i : db->GetCatalog().IndexesOf("u")) names.push_back(i->name);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"u_a_b_key", "u_c_key"})) << "the repeated UNIQUE (c) adds nothing";

    db->Execute("INSERT INTO u VALUES (1, NULL, 1), (1, NULL, 2), (NULL, 'x', 3), (NULL, 'x', 4)");
    db->Execute("INSERT INTO u VALUES (1, 'x', 5), (2, 'x', 6), (1, 'y', 7)");
    EXPECT_EQ(ErrorOf(*db, "INSERT INTO u VALUES (1, 'x', 8)"),
              "duplicate key value violates unique constraint \"u_a_b_key\": (a, b)=(1, x)");
    EXPECT_THROW(db->Execute("UPDATE u SET b = 'x' WHERE c = 7"), DbError);
    EXPECT_EQ(Msg("UPDATE u SET b = NULL WHERE c IN (5, 7)"), "UPDATE 2") << "(1, NULL) twice is allowed";
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM u WHERE a = 1 AND b IS NULL"), 4);
    ExpectIndexesConsistent("u");
}

TEST_F(SqlTest, CreateMultiColumnIndexOnExistingRows) {
    db->Execute("CREATE TABLE t (a INT, b TEXT, c INT)");
    db->Execute("INSERT INTO t VALUES (1, 'x', 1), (1, 'y', 2), (2, 'x', 3), (1, 'x', 4)");
    EXPECT_EQ(ErrorOf(*db, "CREATE UNIQUE INDEX t_ab ON t (a, b)"),
              "could not create unique index \"t_ab\": duplicate key (a, b)=(1, x)");
    EXPECT_EQ(Msg("CREATE UNIQUE INDEX t_abc ON t (a, b, c)"), "CREATE INDEX");
    EXPECT_EQ(Msg("CREATE INDEX t_ba ON t (b, a)"), "CREATE INDEX");
    EXPECT_THROW(db->Execute("INSERT INTO t VALUES (1, 'x', 4)"), DbError);
    EXPECT_EQ(Msg("INSERT INTO t VALUES (1, 'x', 5)"), "INSERT 1");
    ExpectIndexesConsistent("t");
    // ALTER TABLE: renaming a key column keeps the index; dropping one drops it.
    db->Execute("ALTER TABLE t RENAME COLUMN b TO bb");
    EXPECT_THROW(db->Execute("INSERT INTO t VALUES (1, 'x', 4)"), DbError) << "still enforced after the rename";
    db->Execute("ALTER TABLE t DROP COLUMN c");
    ASSERT_EQ(db->GetCatalog().IndexesOf("t").size(), 1u);
    EXPECT_EQ(db->GetCatalog().IndexesOf("t")[0]->name, "t_ba");
    EXPECT_EQ(db->GetCatalog().IndexesOf("t")[0]->columns, (std::vector<std::string>{"bb", "a"}));
    ExpectIndexesConsistent("t");
}

TEST_F(SqlTest, PlannerUsesMultiColumnIndexes) {
    db->Execute("CREATE TABLE ol (order_id INT, line INT, item TEXT, qty INT, PRIMARY KEY (order_id, line))");
    std::string sql = "INSERT INTO ol VALUES ";
    for (int o = 1; o <= 200; ++o)
        for (int l = 1; l <= 10; ++l)
            sql += (o + l > 2 ? ", (" : "(") + std::to_string(o) + ", " + std::to_string(l) + ", 'item" +
                   std::to_string(o % 50) + "', " + std::to_string(l * o % 7) + ")";
    db->Execute(sql);
    db->Execute("CREATE INDEX ol_item_qty ON ol (item, qty)");
    auto plan = [&](const std::string& where) {
        std::string out;
        for (auto& r : Q("EXPLAIN SELECT * FROM ol WHERE " + where)) out += r[0].AsText() + "\n";
        return out;
    };
    auto has = [](const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    };

    std::string p = plan("order_id = 7 AND line = 3");
    EXPECT_TRUE(has(p, "Index Scan using ol_pkey on ol  Index Cond: (ol.order_id, ol.line) = (7, 3)")) << p;
    EXPECT_EQ(Q("SELECT order_id, line FROM ol WHERE order_id = 7 AND line = 3").size(), 1u);

    p = plan("order_id = 7");
    EXPECT_TRUE(has(p, "Index Cond: ol.order_id = 7")) << p;
    EXPECT_EQ(Q("SELECT * FROM ol WHERE order_id = 7").size(), 10u);

    p = plan("order_id = 7 AND line >= 3 AND line < 6");
    EXPECT_TRUE(has(p, "Index Cond: (ol.order_id, ol.line) >= (7, 3) AND (ol.order_id, ol.line) < (7, 6)")) << p;
    auto lines = Q("SELECT line FROM ol WHERE order_id = 7 AND line >= 3 AND line < 6");
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0][0].AsInt(), 3) << "index order";

    p = plan("order_id BETWEEN 5 AND 6");
    EXPECT_TRUE(has(p, "Index Scan using ol_pkey")) << p;
    EXPECT_EQ(Q("SELECT * FROM ol WHERE order_id BETWEEN 5 AND 6").size(), 20u);

    p = plan("line = 3");
    EXPECT_TRUE(has(p, "Seq Scan on ol")) << "no condition on the leading column: " << p;

    // (TEXT, INT): the leading column is hashed, so its condition stays as a re-check.
    p = plan("item = 'item7' AND qty = 2");
    EXPECT_TRUE(has(p, "Index Scan using ol_item_qty")) << p;
    EXPECT_TRUE(has(p, "(hash(ol.item), ol.qty) = (")) << p;
    EXPECT_TRUE(has(p, "Filter: ")) << p;
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM ol WHERE item = 'item7' AND qty = 2"),
              Scalar("SELECT COUNT(*) FROM ol WHERE item || '' = 'item7' AND qty + 0 = 2"));
}

// Differential test: random data, random modifications and random queries over a
// table with multi-column indexes must give the same results as over a copy
// without any index (always a sequential scan).
TEST_F(SqlTest, MultiColumnIndexesMatchSequentialScans) {
    db->Execute("CREATE TABLE r (id INT PRIMARY KEY, a INT, b INT, c TEXT, d INT)");
    db->Execute("CREATE TABLE plain (id INT, a INT, b INT, c TEXT, d INT)");
    db->Execute("CREATE INDEX r_ab ON r (a, b); CREATE INDEX r_ca ON r (c, a); CREATE INDEX r_acd ON r (a, c, d)");
    db->Execute("CREATE UNIQUE INDEX r_db ON r (d, b, id)");
    std::mt19937 rng(2024);
    auto small = [&](int n) { return rng() % 9 == 0 ? std::string("NULL") : std::to_string(static_cast<int>(rng() % n) - 2); };
    auto text = [&] { return rng() % 9 == 0 ? std::string("NULL") : "'s" + std::to_string(rng() % 4) + "'"; };
    std::string values;
    for (int id = 1; id <= 1500; ++id)
        values += (id > 1 ? ", (" : "(") + std::to_string(id) + ", " + small(60) + ", " + small(12) + ", " + text() +
                  ", " + small(5) + ")";
    db->Execute("INSERT INTO r VALUES " + values + "; INSERT INTO plain SELECT * FROM r");

    auto cond = [&] {
        static const char* ops[] = {"=", "=", "<", "<=", ">", ">="};
        switch (rng() % 4) {
            case 0: return "a " + std::string(ops[rng() % 6]) + " " + std::to_string(static_cast<int>(rng() % 62) - 3);
            case 1: return "b " + std::string(ops[rng() % 6]) + " " + std::to_string(static_cast<int>(rng() % 14) - 3);
            case 2: return "c = 's" + std::to_string(rng() % 5) + "'";
            default: return "d = " + std::to_string(static_cast<int>(rng() % 6) - 2);
        }
    };
    auto sorted = [&](const std::string& sql) {
        std::vector<std::string> out;
        for (auto& row : Q(sql)) {
            std::string s;
            for (auto& v : row) s += v.ToString() + "|";
            out.push_back(s);
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    int index_scans = 0;
    for (int round = 0; round < 6; ++round) {
        for (int q = 0; q < 60; ++q) {
            std::string where = cond();
            for (int k = rng() % 3; k > 0; --k) where += " AND " + cond();
            std::string sel = "SELECT id, a, b, c, d FROM ";
            ASSERT_EQ(sorted(sel + "r WHERE " + where), sorted(sel + "plain WHERE " + where)) << where;
            for (auto& line : Q("EXPLAIN " + sel + "r WHERE " + where))
                index_scans += line[0].AsText().find("Index Scan") != std::string::npos;
        }
        // Change both tables the same way, then check again.
        std::string where = cond();
        std::string set = "a = " + small(60) + ", b = b + 1, c = " + text();
        db->Execute("UPDATE r SET " + set + " WHERE " + where + "; UPDATE plain SET " + set + " WHERE " + where);
        where = cond() + " AND " + cond();
        db->Execute("DELETE FROM r WHERE " + where + "; DELETE FROM plain WHERE " + where);
        ExpectIndexesConsistent("r");
    }
    EXPECT_GT(index_scans, 50) << "the multi-column indexes were actually used";
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM r"), Scalar("SELECT COUNT(*) FROM plain"));
}

}  // namespace
}  // namespace minidb
