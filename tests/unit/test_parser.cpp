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

TEST(Parser, LeftJoinKeepsItsOnConditionSeparate) {
    auto s = ParseOne<SelectStmt>("SELECT * FROM a x LEFT OUTER JOIN b ON x.k = b.k JOIN c ON c.k = b.k WHERE x.v > 1");
    ASSERT_EQ(s.from.size(), 3u);
    EXPECT_EQ(s.from[0].join, JoinType::INNER);
    EXPECT_EQ(s.from[1].join, JoinType::LEFT);
    ASSERT_TRUE(s.from[1].on);
    EXPECT_EQ(s.from[1].on->ToString(), "x.k = b.k");
    EXPECT_EQ(s.from[2].join, JoinType::INNER);
    EXPECT_FALSE(s.from[2].on);
    EXPECT_EQ(s.where->ToString(), "(c.k = b.k) AND (x.v > 1)") << "inner ON joins WHERE, LEFT ON does not";
    EXPECT_EQ(ParseOne<SelectStmt>("SELECT * FROM a LEFT JOIN b ON TRUE").from[1].join, JoinType::LEFT);
}

TEST(Parser, RightAndFullJoins) {
    auto s = ParseOne<SelectStmt>("SELECT * FROM a RIGHT OUTER JOIN b ON a.k = b.k FULL JOIN c ON c.k = b.k");
    ASSERT_EQ(s.from.size(), 3u);
    EXPECT_EQ(s.from[1].join, JoinType::RIGHT);
    EXPECT_EQ(s.from[1].on->ToString(), "a.k = b.k");
    EXPECT_EQ(s.from[2].join, JoinType::FULL);
    EXPECT_EQ(s.from[2].on->ToString(), "c.k = b.k");
    EXPECT_EQ(s.where, nullptr);
    EXPECT_EQ(ParseOne<SelectStmt>("SELECT * FROM a FULL OUTER JOIN b ON TRUE").from[1].join, JoinType::FULL);
    EXPECT_THROW(Parser("SELECT * FROM a RIGHT JOIN b").ParseAll(), DbError) << "ON is required";
    EXPECT_THROW(Parser("SELECT * FROM a FULL b ON TRUE").ParseAll(), DbError) << "JOIN is required";
}

TEST(Parser, InnerOnBeforeRightOrFullJoinStaysWithItsJoin) {
    // (a JOIN b ON ab) RIGHT JOIN c: ab must filter before the RIGHT JOIN keeps
    // every c row, so it cannot become a WHERE condition. Inner ONs after the
    // last RIGHT / FULL JOIN still go to WHERE.
    auto s = ParseOne<SelectStmt>(
        "SELECT * FROM a JOIN b ON a.k = b.k RIGHT JOIN c ON c.k = b.k JOIN d ON d.k = c.k WHERE a.v > 1");
    ASSERT_EQ(s.from.size(), 4u);
    ASSERT_TRUE(s.from[1].on);
    EXPECT_EQ(s.from[1].join, JoinType::INNER);
    EXPECT_EQ(s.from[1].on->ToString(), "a.k = b.k");
    EXPECT_FALSE(s.from[3].on);
    EXPECT_EQ(s.where->ToString(), "(d.k = c.k) AND (a.v > 1)");
}

TEST(Parser, RightOrFullJoinAfterACommaIsRejected) {
    // "a, b RIGHT JOIN c" is a × (b RIGHT JOIN c), not (a × b) RIGHT JOIN c.
    EXPECT_THROW(Parser("SELECT * FROM a, b RIGHT JOIN c ON b.k = c.k").ParseAll(), DbError);
    EXPECT_THROW(Parser("SELECT * FROM a, b FULL JOIN c ON b.k = c.k").ParseAll(), DbError);
    EXPECT_NO_THROW(Parser("SELECT * FROM a CROSS JOIN b RIGHT JOIN c ON b.k = c.k").ParseAll());
    EXPECT_NO_THROW(Parser("SELECT * FROM a RIGHT JOIN b ON a.k = b.k, c").ParseAll());
}

TEST(Parser, JoinKeywordsAreNotTableAliases) {
    // "FROM a left join b" used to read "left" as an alias of a.
    EXPECT_EQ(ParseOne<SelectStmt>("SELECT * FROM a LEFT JOIN b ON a.k = b.k").from[0].alias, "");
    EXPECT_THROW(Parser("SELECT * FROM a LEFT JOIN b").ParseAll(), DbError) << "ON is required";
    EXPECT_EQ(ParseOne<SelectStmt>("SELECT * FROM a RIGHT JOIN b ON a.k = b.k").from[0].alias, "");
    EXPECT_EQ(ParseOne<SelectStmt>("SELECT * FROM a FULL JOIN b ON a.k = b.k").from[0].alias, "");
    EXPECT_EQ(ParseExpr("\"left\"")->column, "left") << "quoted, the word is still a valid name";
}

TEST(Parser, ScalarFunctionsCaseAndConcat) {
    EXPECT_EQ(ParseExpr("UPPER(name)")->ToString(), "upper(name)");
    EXPECT_EQ(ParseExpr("COALESCE(a, b, 0)")->args.size(), 3u);
    EXPECT_EQ(ParseExpr("'x' || 1 + 2")->ToString(), "'x' || (1 + 2)") << "|| binds looser than +";
    EXPECT_EQ(ParseExpr("a || b = 'ab'")->ToString(), "(a || b) = 'ab'") << "and tighter than =";
    EXPECT_EQ(ParseExpr("CASE WHEN x > 1 THEN 'big' ELSE 'small' END")->ToString(),
              "CASE WHEN x > 1 THEN 'big' ELSE 'small' END");
    EXPECT_EQ(ParseExpr("CASE x WHEN 1 THEN 'one' WHEN 2 THEN 'two' END")->ToString(),
              "CASE WHEN x = 1 THEN 'one' WHEN x = 2 THEN 'two' END")
        << "simple CASE becomes a searched CASE";
    EXPECT_THROW(ParseExpr("CASE END"), DbError);
    EXPECT_THROW(ParseExpr("CASE WHEN x THEN 1"), DbError) << "missing END";
}

TEST(Parser, FunctionArityAndUnknownFunctions) {
    EXPECT_THROW(ParseExpr("ABS()"), DbError);
    EXPECT_THROW(ParseExpr("ABS(1, 2)"), DbError);
    EXPECT_THROW(ParseExpr("SUBSTR('a')"), DbError);
    EXPECT_NO_THROW(ParseExpr("SUBSTR('a', 1)"));
    EXPECT_NO_THROW(ParseExpr("ROUND(1.5, 1)"));
    EXPECT_THROW(ParseExpr("FROBNICATE(1)"), DbError);
}

TEST(Parser, Subqueries) {
    auto scalar = ParseExpr("(SELECT MAX(x) FROM t) + 1");
    ASSERT_EQ(scalar->kind, ExprKind::BINARY);
    EXPECT_EQ(scalar->left->kind, ExprKind::SUBQUERY);
    EXPECT_EQ(scalar->left->sub_kind, SubqueryKind::SCALAR);
    EXPECT_EQ(scalar->left->subquery->from.at(0).name, "t");

    auto in = ParseExpr("a NOT IN (SELECT b FROM u WHERE c > 1)");
    ASSERT_EQ(in->kind, ExprKind::SUBQUERY);
    EXPECT_EQ(in->sub_kind, SubqueryKind::IN);
    EXPECT_TRUE(in->negated);
    EXPECT_EQ(in->left->ToString(), "a");
    EXPECT_TRUE(in->subquery->where);

    auto ex = ParseExpr("NOT EXISTS (SELECT 1 FROM u)");
    ASSERT_EQ(ex->kind, ExprKind::UNARY);
    EXPECT_EQ(ex->left->sub_kind, SubqueryKind::EXISTS);
    EXPECT_EQ(ParseExpr("a IN (1, 2)")->kind, ExprKind::BINARY) << "a value list is still desugared";

    auto nested = ParseExpr("a IN (SELECT b FROM u WHERE b IN (SELECT c FROM v))");
    EXPECT_EQ(nested->subquery->where->kind, ExprKind::SUBQUERY);
    EXPECT_THROW(ParseExpr("EXISTS SELECT 1"), DbError);
    EXPECT_THROW(ParseExpr("(SELECT 1"), DbError);
}

TEST(Parser, ColumnDefaults) {
    auto c = ParseOne<CreateTableStmt>("CREATE TABLE t (a INT DEFAULT -1 NOT NULL, b TEXT DEFAULT 'x' || 'y', c INT)");
    ASSERT_EQ(c.cols.size(), 3u);
    ASSERT_TRUE(c.cols[0].default_expr);
    EXPECT_EQ(c.cols[0].default_expr->value.AsInt(), -1);
    EXPECT_TRUE(c.cols[0].not_null) << "NOT NULL after DEFAULT is still a constraint";
    EXPECT_EQ(c.cols[1].default_expr->ToString(), "'x' || 'y'");
    EXPECT_FALSE(c.cols[2].default_expr);
    EXPECT_TRUE(ParseOne<AlterTableStmt>("ALTER TABLE t ADD n INT DEFAULT 0").column.default_expr);
    EXPECT_THROW(Parser("CREATE TABLE t (a INT DEFAULT)").ParseAll(), DbError);
}

TEST(Parser, AlterTable) {
    using A = AlterTableStmt::Action;
    auto add = ParseOne<AlterTableStmt>("ALTER TABLE t ADD COLUMN email TEXT UNIQUE");
    EXPECT_EQ(add.table, "t");
    EXPECT_EQ(add.action, A::ADD_COLUMN);
    EXPECT_EQ(add.column.name, "email");
    EXPECT_EQ(add.column.type, TypeId::TEXT);
    EXPECT_TRUE(add.column.unique);
    EXPECT_EQ(ParseOne<AlterTableStmt>("ALTER TABLE t ADD n INT NOT NULL").column.name, "n") << "COLUMN is optional";
    EXPECT_EQ(ParseOne<AlterTableStmt>("ALTER TABLE t ADD column INT").column.name, "column")
        << "a column may be called \"column\"";
    auto drop = ParseOne<AlterTableStmt>("ALTER TABLE t DROP COLUMN x");
    EXPECT_EQ(drop.action, A::DROP_COLUMN);
    EXPECT_EQ(drop.name, "x");
    EXPECT_EQ(ParseOne<AlterTableStmt>("ALTER TABLE t DROP column").name, "column");
    auto rc = ParseOne<AlterTableStmt>("ALTER TABLE t RENAME COLUMN a TO b");
    EXPECT_EQ(rc.action, A::RENAME_COLUMN);
    EXPECT_EQ(rc.name, "a");
    EXPECT_EQ(rc.new_name, "b");
    EXPECT_EQ(ParseOne<AlterTableStmt>("ALTER TABLE t RENAME a TO b").new_name, "b");
    EXPECT_EQ(ParseOne<AlterTableStmt>("ALTER TABLE t RENAME column TO c").name, "column");
    auto rt = ParseOne<AlterTableStmt>("ALTER TABLE t RENAME TO u");
    EXPECT_EQ(rt.action, A::RENAME_TABLE);
    EXPECT_EQ(rt.new_name, "u");
    EXPECT_THROW(Parser("ALTER TABLE t").ParseAll(), DbError);
    EXPECT_THROW(Parser("ALTER TABLE t MODIFY x INT").ParseAll(), DbError);
    EXPECT_THROW(Parser("ALTER TABLE t ADD x BLOB").ParseAll(), DbError);
}

TEST(Parser, DerivedTablesInFrom) {
    auto s = ParseOne<SelectStmt>("SELECT * FROM (SELECT a FROM t) AS x JOIN (SELECT b FROM u UNION SELECT 1) y ON x.a = y.b");
    ASSERT_EQ(s.from.size(), 2u);
    ASSERT_TRUE(s.from[0].subquery);
    EXPECT_EQ(s.from[0].alias, "x");
    EXPECT_EQ(s.from[0].subquery->from.at(0).name, "t");
    ASSERT_TRUE(s.from[1].subquery);
    EXPECT_TRUE(s.from[1].subquery->set_lhs) << "a compound query inside FROM";
    EXPECT_EQ(s.from[1].alias, "y");
    EXPECT_FALSE(ParseOne<SelectStmt>("SELECT * FROM t").from[0].subquery);
    EXPECT_THROW(Parser("SELECT * FROM (SELECT 1)").ParseAll(), DbError) << "alias required";
    EXPECT_THROW(Parser("SELECT * FROM (t) x").ParseAll(), DbError);
}

TEST(Parser, SetOperationsAndPrecedence) {
    auto s = ParseOne<SelectStmt>("SELECT a FROM t UNION ALL SELECT b FROM u ORDER BY 1 LIMIT 5");
    ASSERT_TRUE(s.set_lhs && s.set_rhs);
    EXPECT_EQ(s.set_op, SetOp::UNION);
    EXPECT_TRUE(s.set_all);
    EXPECT_EQ(s.order_by.size(), 1u) << "ORDER BY / LIMIT belong to the whole result";
    EXPECT_EQ(s.limit, 5);
    EXPECT_TRUE(s.set_rhs->order_by.empty());
    EXPECT_EQ(s.set_lhs->from.at(0).name, "t");

    // INTERSECT binds tighter: A UNION (B INTERSECT C).
    auto p = ParseOne<SelectStmt>("SELECT 1 UNION SELECT 2 INTERSECT SELECT 3");
    EXPECT_EQ(p.set_op, SetOp::UNION);
    ASSERT_TRUE(p.set_rhs->set_lhs);
    EXPECT_EQ(p.set_rhs->set_op, SetOp::INTERSECT);
    // UNION and EXCEPT are left-associative: (A EXCEPT B) UNION C.
    auto q = ParseOne<SelectStmt>("SELECT 1 EXCEPT SELECT 2 UNION DISTINCT SELECT 3");
    EXPECT_EQ(q.set_op, SetOp::UNION);
    EXPECT_FALSE(q.set_all);
    ASSERT_TRUE(q.set_lhs->set_lhs);
    EXPECT_EQ(q.set_lhs->set_op, SetOp::EXCEPT);

    EXPECT_THROW(Parser("SELECT 1 UNION").ParseAll(), DbError);
    EXPECT_THROW(Parser("SELECT 1 ORDER BY 1 UNION SELECT 2").ParseAll(), DbError)
        << "ORDER BY must come after the last query";
}

TEST(Parser, DistinctAggregates) {
    auto e = ParseExpr("COUNT(DISTINCT x)");
    ASSERT_EQ(e->kind, ExprKind::AGG);
    EXPECT_TRUE(e->agg_distinct);
    EXPECT_EQ(e->ToString(), "count(DISTINCT x)");
    EXPECT_FALSE(ParseExpr("COUNT(x)")->agg_distinct);
    EXPECT_TRUE(ParseExpr("SUM(DISTINCT x)")->agg_distinct);
    EXPECT_THROW(ParseExpr("COUNT(DISTINCT *)"), DbError);
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

// Input nested deeper than the stack can take must be a syntax error, never a crash.
std::string Repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

TEST(Parser, DeepNestingIsRejectedNotACrash) {
    EXPECT_EQ(ParseExpr(Repeat("(", 200) + "1" + Repeat(")", 200))->value.AsInt(), 1);
    EXPECT_THROW(ParseExpr(Repeat("(", 100000) + "1" + Repeat(")", 100000)), DbError);
    EXPECT_THROW(ParseExpr(Repeat("- ", 100000) + "1"), DbError);
    EXPECT_THROW(ParseExpr(Repeat("+ ", 100000) + "1"), DbError);
    EXPECT_THROW(ParseExpr(Repeat("NOT ", 100000) + "TRUE"), DbError);
    EXPECT_EQ(ParseExpr(Repeat("- ", 100) + "1")->value.AsInt(), 1) << "moderate nesting is fine";
}

TEST(Parser, LongOperatorChainsAreLimitedByTreeHeight) {
    std::string ok = "1";
    for (int i = 2; i <= kMaxExprHeight; ++i) ok += " + " + std::to_string(i);  // 1000 terms, 999 "+" nodes
    EXPECT_EQ(ParseExpr(ok)->height, kMaxExprHeight);
    EXPECT_THROW(ParseExpr(ok + " + 1"), DbError);
}

TEST(Parser, LargeInListIsABalancedTree) {
    std::string list;
    for (int i = 0; i < 50000; ++i) list += (i ? "," : "") + std::to_string(i);
    auto e = ParseExpr("x IN (" + list + ")");
    EXPECT_LE(e->height, 20) << "50000 values -> ~log2(50000) = 16 levels of OR, not 50000";
    EXPECT_LE(ParseExpr("x NOT IN (" + list + ")")->height, 21);
}

TEST(Parser, CreateView) {
    auto v = ParseOne<CreateViewStmt>("create view v as select a, 'x;y' from t where a > 1");
    EXPECT_EQ(v.name, "v");
    EXPECT_FALSE(v.or_replace);
    EXPECT_TRUE(v.columns.empty());
    ASSERT_NE(v.select, nullptr);
    EXPECT_EQ(v.select->from.at(0).name, "t");
    EXPECT_EQ(v.sql, "select a, 'x;y' from t where a > 1") << "the query's text exactly as written";

    auto r = ParseOne<CreateViewStmt>("CREATE OR REPLACE VIEW \"V\" (x, y) AS\n  SELECT 1, 2 UNION SELECT 3, 4 -- note\n");
    EXPECT_TRUE(r.or_replace);
    EXPECT_EQ(r.name, "V");
    EXPECT_EQ(r.columns, (std::vector<std::string>{"x", "y"}));
    ASSERT_NE(r.select->set_lhs, nullptr);
    EXPECT_EQ(r.sql, "SELECT 1, 2 UNION SELECT 3, 4") << "a trailing comment is not part of the query";

    // The stored text parses back into the same query.
    SelectStmt again = Parser::ParseQuery(r.sql);
    EXPECT_NE(again.set_lhs, nullptr);
    EXPECT_THROW(Parser::ParseQuery("SELECT 1; SELECT 2"), DbError);
    EXPECT_THROW(Parser::ParseQuery("DELETE FROM t"), DbError);

    // "view" and "replace" are not reserved: they still work as names.
    auto t = ParseOne<CreateTableStmt>("CREATE TABLE view (replace INT)");
    EXPECT_EQ(t.name, "view");
    EXPECT_EQ(t.cols.at(0).name, "replace");
}

TEST(Parser, CreateViewSyntaxErrors) {
    for (const char* sql : {"CREATE VIEW v SELECT 1", "CREATE VIEW v AS DELETE FROM t", "CREATE VIEW AS SELECT 1",
                            "CREATE VIEW v () AS SELECT 1", "CREATE OR VIEW v AS SELECT 1",
                            "CREATE OR REPLACE TABLE t (a INT)", "CREATE UNIQUE VIEW v AS SELECT 1"})
        EXPECT_THROW(Parser(sql).ParseAll(), DbError) << sql;
}

TEST(Parser, DropView) {
    auto d = ParseOne<DropStmt>("DROP VIEW v");
    EXPECT_TRUE(d.view);
    EXPECT_FALSE(d.index);
    EXPECT_EQ(d.name, "v");
    EXPECT_FALSE(ParseOne<DropStmt>("DROP TABLE t").view);
    EXPECT_THROW(Parser("DROP SEQUENCE s").ParseAll(), DbError);
}

}  // namespace
}  // namespace minidb
