// Unit tests: SQL parser (text -> AST). No execution involved.
#include "parser.h"
#include "test_util.h"

namespace minidb {
namespace {

template <typename T>
T ParseOne(const std::string& sql) {
    auto stmts = Parser(sql).ParseAll();
    EXPECT_EQ(stmts.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<T>(stmts.at(0))) << sql;
    return std::get<T>(stmts.at(0));
}

ExprPtr ParseExpr(const std::string& expr) {
    auto s = ParseOne<SelectStmt>("SELECT " + expr);
    return s.items.at(0).expr;
}

TEST(Parser, CreateTable) {
    auto c = ParseOne<CreateTableStmt>(
        "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL, score DOUBLE PRECISION, "
        "ok BOOLEAN, code INT UNIQUE, note TEXT NULL)");
    EXPECT_EQ(c.name, "t");
    ASSERT_EQ(c.cols.size(), 6u);
    EXPECT_TRUE(c.cols[0].primary_key);
    EXPECT_TRUE(c.cols[0].not_null);
    EXPECT_EQ(c.cols[1].type, TypeId::TEXT);
    EXPECT_TRUE(c.cols[1].not_null);
    EXPECT_EQ(c.cols[2].type, TypeId::DOUBLE);
    EXPECT_EQ(c.cols[3].type, TypeId::BOOL);
    EXPECT_TRUE(c.cols[4].unique);
    EXPECT_FALSE(c.cols[5].not_null);
}

TEST(Parser, TableLevelPrimaryKey) {
    auto c = ParseOne<CreateTableStmt>("CREATE TABLE t (a INT, b INT, PRIMARY KEY (b))");
    EXPECT_FALSE(c.cols[0].primary_key);
    EXPECT_TRUE(c.cols[1].primary_key);
    EXPECT_THROW(Parser("CREATE TABLE t (a INT, PRIMARY KEY (zz))").ParseAll(), DbError);
}

TEST(Parser, UnknownTypeIsRejected) { EXPECT_THROW(Parser("CREATE TABLE t (a BLOB)").ParseAll(), DbError); }

TEST(Parser, CreateIndexAndDrop) {
    auto i = ParseOne<CreateIndexStmt>("CREATE UNIQUE INDEX idx ON t (col)");
    EXPECT_TRUE(i.unique);
    EXPECT_EQ(i.name, "idx");
    EXPECT_EQ(i.table, "t");
    EXPECT_EQ(i.column, "col");
    EXPECT_FALSE(ParseOne<CreateIndexStmt>("CREATE INDEX i ON t (c)").unique);
    EXPECT_FALSE(ParseOne<DropStmt>("DROP TABLE t").index);
    EXPECT_TRUE(ParseOne<DropStmt>("DROP INDEX i").index);
}

TEST(Parser, InsertValues) {
    auto s = ParseOne<InsertStmt>("INSERT INTO t (a, b) VALUES (1, 'x'), (2, NULL)");
    EXPECT_EQ(s.table, "t");
    EXPECT_EQ(s.columns, (std::vector<std::string>{"a", "b"}));
    ASSERT_EQ(s.rows.size(), 2u);
    EXPECT_EQ(s.rows[0][1]->value.AsText(), "x");
    EXPECT_TRUE(s.rows[1][1]->value.IsNull());
    EXPECT_EQ(s.select, nullptr);
}

TEST(Parser, InsertSelect) {
    auto s = ParseOne<InsertStmt>("INSERT INTO t SELECT * FROM u");
    ASSERT_NE(s.select, nullptr);
    EXPECT_EQ(s.select->from[0].name, "u");
}

TEST(Parser, FullSelect) {
    auto s = ParseOne<SelectStmt>(
        "SELECT DISTINCT a.x AS ax, y total, b.* FROM t1 a JOIN t2 AS b ON a.id = b.id, t3 "
        "WHERE a.x > 1 GROUP BY a.x, y HAVING COUNT(*) > 2 ORDER BY ax DESC, y LIMIT 10 OFFSET 5");
    EXPECT_TRUE(s.distinct);
    ASSERT_EQ(s.items.size(), 3u);
    EXPECT_EQ(s.items[0].alias, "ax");
    EXPECT_EQ(s.items[1].alias, "total");
    EXPECT_TRUE(s.items[2].star);
    EXPECT_EQ(s.items[2].star_table, "b");
    ASSERT_EQ(s.from.size(), 3u);
    EXPECT_EQ(s.from[0].alias, "a");
    EXPECT_EQ(s.from[1].alias, "b");
    EXPECT_EQ(s.from[2].alias, "");
    // JOIN ... ON condition is folded into WHERE
    EXPECT_EQ(s.where->ToString(), "(a.id = b.id) AND (a.x > 1)");
    EXPECT_EQ(s.group_by.size(), 2u);
    EXPECT_EQ(s.having->ToString(), "count(*) > 2");
    ASSERT_EQ(s.order_by.size(), 2u);
    EXPECT_TRUE(s.order_by[0].desc);
    EXPECT_FALSE(s.order_by[1].desc);
    EXPECT_EQ(s.limit, 10);
    EXPECT_EQ(s.offset, 5);
}

TEST(Parser, CrossJoinHasNoCondition) {
    auto s = ParseOne<SelectStmt>("SELECT * FROM a CROSS JOIN b");
    EXPECT_EQ(s.from.size(), 2u);
    EXPECT_EQ(s.where, nullptr);
}

TEST(Parser, OperatorPrecedence) {
    EXPECT_EQ(ParseExpr("1 + 2 * 3")->ToString(), "1 + (2 * 3)");
    EXPECT_EQ(ParseExpr("(1 + 2) * 3")->ToString(), "(1 + 2) * 3");
    EXPECT_EQ(ParseExpr("a OR b AND c")->ToString(), "a OR (b AND c)");
    EXPECT_EQ(ParseExpr("NOT a = 1")->ToString(), "NOT (a = 1)");
    EXPECT_EQ(ParseExpr("a + 1 < b * 2")->ToString(), "(a + 1) < (b * 2)");
    EXPECT_EQ(ParseExpr("10 - 4 - 3")->ToString(), "(10 - 4) - 3");  // left associative
}

TEST(Parser, UnaryMinusFoldsConstants) {
    auto e = ParseExpr("-5");
    EXPECT_EQ(e->kind, ExprKind::CONST);
    EXPECT_EQ(e->value.AsInt(), -5);
    EXPECT_EQ(ParseExpr("-x")->kind, ExprKind::UNARY);
}

TEST(Parser, SyntacticSugarIsDesugared) {
    EXPECT_EQ(ParseExpr("x BETWEEN 1 AND 5")->ToString(), "(x >= 1) AND (x <= 5)");
    EXPECT_EQ(ParseExpr("x IN (1, 2, 3)")->ToString(), "((x = 1) OR (x = 2)) OR (x = 3)");
    EXPECT_EQ(ParseExpr("x NOT IN (1)")->ToString(), "NOT (x = 1)");
    EXPECT_EQ(ParseExpr("x NOT LIKE 'a%'")->ToString(), "NOT (x LIKE 'a%')");
    EXPECT_EQ(ParseExpr("x IS NOT NULL")->ToString(), "x IS NOT NULL");
}

TEST(Parser, Literals) {
    EXPECT_EQ(ParseExpr("TRUE")->value.AsBool(), true);
    EXPECT_TRUE(ParseExpr("NULL")->value.IsNull());
    EXPECT_DOUBLE_EQ(ParseExpr("2.5")->value.AsDouble(), 2.5);
    EXPECT_THROW(ParseExpr("99999999999999999999"), DbError);
    EXPECT_THROW(ParseExpr("1e400"), DbError) << "must be a DbError, not std::out_of_range";
}

TEST(Parser, IntegerLiteralLimits) {
    EXPECT_EQ(ParseExpr("9223372036854775807")->value.AsInt(), INT64_MAX);
    EXPECT_EQ(ParseExpr("-9223372036854775807")->value.AsInt(), -INT64_MAX);
    auto min = ParseExpr("-9223372036854775808");
    ASSERT_EQ(min->kind, ExprKind::CONST);
    EXPECT_EQ(min->value.AsInt(), INT64_MIN);
    EXPECT_THROW(ParseExpr("9223372036854775808"), DbError) << "only valid with a minus sign";
    EXPECT_THROW(ParseExpr("- -9223372036854775808"), DbError) << "negating INT64_MIN overflows";
    EXPECT_EQ(ParseExpr("- -5")->value.AsInt(), 5);
}

TEST(Parser, Aggregates) {
    auto e = ParseExpr("COUNT(*)");
    EXPECT_EQ(e->kind, ExprKind::AGG);
    EXPECT_TRUE(e->agg_star);
    e = ParseExpr("avg(price * 2)");
    EXPECT_EQ(e->agg, AggFunc::AVG);
    EXPECT_EQ(e->ToString(), "avg(price * 2)");
    EXPECT_THROW(ParseExpr("frobnicate(1)"), DbError);
    EXPECT_THROW(ParseExpr("SUM(*)"), DbError);
}

TEST(Parser, UpdateAndDelete) {
    auto u = ParseOne<UpdateStmt>("UPDATE t SET a = a + 1, b = 'x' WHERE id = 3");
    EXPECT_EQ(u.table, "t");
    ASSERT_EQ(u.sets.size(), 2u);
    EXPECT_EQ(u.sets[0].first, "a");
    EXPECT_EQ(u.where->ToString(), "id = 3");
    auto d = ParseOne<DeleteStmt>("DELETE FROM t");
    EXPECT_EQ(d.where, nullptr);
}

TEST(Parser, Explain) {
    auto e = ParseOne<ExplainStmt>("EXPLAIN ANALYZE SELECT 1");
    EXPECT_TRUE(e.analyze);
    EXPECT_FALSE(ParseOne<ExplainStmt>("EXPLAIN SELECT 1").analyze);
    EXPECT_THROW(Parser("EXPLAIN DELETE FROM t").ParseAll(), DbError);
}

TEST(Parser, MultipleStatements) {
    auto stmts = Parser("SELECT 1; ;SELECT 2;").ParseAll();
    EXPECT_EQ(stmts.size(), 2u);
    EXPECT_THROW(Parser("SELECT 1 SELECT 2").ParseAll(), DbError);
    EXPECT_TRUE(Parser("  ").ParseAll().empty());
}

TEST(Parser, SyntaxErrorsAreReported) {
    for (const char* bad : {"SELEC 1", "SELECT FROM t", "SELECT * FROM", "INSERT t VALUES (1)",
                            "CREATE TABLE t ()", "SELECT (1", "UPDATE t a = 1", "SELECT a FROM t WHERE"}) {
        try {
            Parser(bad).ParseAll();
            ADD_FAILURE() << "expected a syntax error for: " << bad;
        } catch (const DbError& e) {
            EXPECT_NE(std::string(e.what()).find("error"), std::string::npos) << e.what();
        }
    }
}

TEST(Parser, ErrorMessageNamesTheOffendingToken) {
    try {
        Parser("SELECT * FROM t WHERE x = = 1").ParseAll();
        FAIL();
    } catch (const DbError& e) {
        EXPECT_NE(std::string(e.what()).find("\"=\""), std::string::npos) << e.what();
    }
}

}  // namespace
}  // namespace minidb
