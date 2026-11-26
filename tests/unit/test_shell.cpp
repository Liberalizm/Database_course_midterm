// Unit tests: the SQL shell (statement splitting, scripts, meta commands, .read).
#include <fstream>
#include <sstream>

#include "shell.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

std::vector<std::string> Split(const std::string& text, std::string* rest = nullptr) {
    std::string r;
    auto out = SplitStatements(text, &r);
    if (rest) *rest = r;
    return out;
}

TEST(SplitStatements, SplitsOnSemicolons) {
    std::string rest;
    auto s = Split("SELECT 1; SELECT 2;\nSELECT", &rest);
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0], "SELECT 1;");
    EXPECT_EQ(s[1], " SELECT 2;");
    EXPECT_EQ(rest, "\nSELECT") << "the unfinished statement is kept for the next line";
}

TEST(SplitStatements, SemicolonsInsideLiteralsIdentifiersAndCommentsDoNotSplit) {
    EXPECT_EQ(Split("SELECT 'a;b';").size(), 1u);
    EXPECT_EQ(Split("SELECT 'it''s; fine';").size(), 1u) << "doubled quote";
    EXPECT_EQ(Split("SELECT 1 AS \"x;y\";").size(), 1u);
    EXPECT_EQ(Split("SELECT 1 AS \"x--y\"; SELECT 2;").size(), 2u) << "-- inside an identifier is no comment";
    EXPECT_EQ(Split("SELECT '--'; SELECT 2;").size(), 2u) << "-- inside a literal is no comment";
    EXPECT_EQ(Split("SELECT 1 -- a; b\n;").size(), 1u) << "';' inside a comment";
}

TEST(SplitStatements, OpenLiteralContinuesOnTheNextLine) {
    std::string rest;
    EXPECT_TRUE(Split("INSERT INTO t VALUES ('a;\n", &rest).empty());
    EXPECT_EQ(rest, "INSERT INTO t VALUES ('a;\n");
    EXPECT_EQ(Split(rest + "b');").size(), 1u);
}

TEST(SplitStatements, EmptyStatementsAreDropped) {
    EXPECT_TRUE(Split(";;  ; -- x\n;").empty());
    EXPECT_EQ(Split("SELECT 1;;").size(), 1u);
}

TEST(HasSqlContent, IgnoresWhitespaceAndComments) {
    EXPECT_FALSE(HasSqlContent(""));
    EXPECT_FALSE(HasSqlContent("  \n\t -- only a comment\n  -- another"));
    EXPECT_TRUE(HasSqlContent("-- c\nSELECT"));
    EXPECT_TRUE(HasSqlContent("'--'"));
}

class ShellTest : public ::testing::Test {
protected:
    TempFile file;
    Database db{file.path(), 32};
    std::ostringstream out;
    Shell shell{db, out};

    std::string Run(const std::string& script) {
        out.str("");
        std::istringstream in(script);
        shell.RunScript(in);
        return out.str();
    }
    static bool Contains(const std::string& haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    }
    // A script file next to the database file.
    std::string WriteScript(const std::string& name, const std::string& text) {
        std::string path = file.path() + "." + name;
        std::ofstream(path, std::ios::binary) << text;
        scripts_.push_back(path);
        return path;
    }
    ~ShellTest() override {
        for (const auto& p : scripts_) std::remove(p.c_str());
    }

private:
    std::vector<std::string> scripts_;
};

TEST_F(ShellTest, StatementsSpanLinesAndShareLines) {
    std::string o = Run("CREATE TABLE t\n  (a INT);\nINSERT INTO t VALUES (1); INSERT INTO t VALUES (2);\nSELECT\nCOUNT(*)\nFROM t;");
    EXPECT_TRUE(Contains(o, "CREATE TABLE\nINSERT 1\nINSERT 1\n")) << o;
    EXPECT_TRUE(Contains(o, " 2\n(1 row)")) << o;
}

TEST_F(ShellTest, AnErrorDoesNotSwallowTheNextStatementOnTheSameLine) {
    std::string o = Run("CREATE TABLE t (a INT);\nSELEC 1; INSERT INTO t VALUES (7); SELECT * FROM nope; SELECT a FROM t;");
    EXPECT_TRUE(Contains(o, "ERROR: syntax error")) << o;
    EXPECT_TRUE(Contains(o, "INSERT 1")) << "the statement after the syntax error still ran:\n" << o;
    EXPECT_TRUE(Contains(o, "ERROR: table \"nope\" does not exist")) << o;
    EXPECT_TRUE(Contains(o, " 7\n(1 row)")) << o;
}

TEST_F(ShellTest, LastStatementWithoutSemicolonStillRuns) {
    EXPECT_TRUE(Contains(Run("SELECT 41 + 1"), " 42\n"));
}

TEST_F(ShellTest, CrLfScriptsWork) {
    std::string o = Run("CREATE TABLE t (a INT);\r\n.tables\r\nSELECT 1;\r\n");
    EXPECT_TRUE(Contains(o, "  t  (0 rows")) << o;
    EXPECT_TRUE(Contains(o, "(1 row)")) << o;
}

TEST_F(ShellTest, MetaCommandsAfterCommentsAndUnknownOnes) {
    std::string o = Run("-- setup\n.timer maybe\n.bogus\nCREATE TABLE t (a INT);\n.schema t\n");
    EXPECT_TRUE(Contains(o, "ERROR: usage: .timer on|off")) << o;
    EXPECT_TRUE(Contains(o, "unknown command .bogus")) << o;
    EXPECT_TRUE(Contains(o, "Table \"t\"\n  a  INT")) << o;
}

TEST_F(ShellTest, ReadRunsAFullScriptWithMetaCommandsAndKeepsGoingAfterErrors) {
    std::string path = WriteScript("setup.sql",
                                   "-- a script\n"
                                   "CREATE TABLE t (a INT PRIMARY KEY);\n"
                                   "INSERT INTO t VALUES (1);\n"
                                   "INSERT INTO t VALUES (1);\n"  // fails: the rest must still run
                                   ".tables\n"
                                   "INSERT INTO t VALUES (2);\n");
    std::string o = Run(".read " + path + "\nSELECT COUNT(*) FROM t;");
    EXPECT_TRUE(Contains(o, "ERROR: duplicate key")) << o;
    EXPECT_TRUE(Contains(o, "  t  (")) << ".tables inside the script ran:\n" << o;
    EXPECT_TRUE(Contains(o, " 2\n(1 row)")) << "both good inserts ran:\n" << o;
}

TEST_F(ShellTest, ReadThatReadsItselfStopsWithAnError) {
    std::string path = file.path() + ".loop.sql";
    WriteScript("loop.sql", ".read " + path + "\n");
    std::string o = Run(".read " + path + "\nSELECT 'alive';");
    EXPECT_TRUE(Contains(o, "nested more than")) << o;
    EXPECT_TRUE(Contains(o, "alive")) << "the shell survives:\n" << o;
}

TEST_F(ShellTest, ReadErrorsAndExit) {
    std::string o = Run(".read\n.read /no/such/file.sql\nSELECT 1;");
    EXPECT_TRUE(Contains(o, "ERROR: usage: .read <file>")) << o;
    EXPECT_TRUE(Contains(o, "ERROR: cannot open file /no/such/file.sql")) << o;

    std::string quit = WriteScript("quit.sql", "SELECT 'before';\n.exit\nSELECT 'after';\n");
    std::istringstream in(".read " + quit + "\nSELECT 'outer';\n");
    out.str("");
    EXPECT_FALSE(shell.RunScript(in)) << ".exit inside a script ends the shell";
    EXPECT_TRUE(Contains(out.str(), "before"));
    EXPECT_FALSE(Contains(out.str(), "after"));
    EXPECT_FALSE(Contains(out.str(), "outer"));
}

}  // namespace
}  // namespace minidb
