// Integration tests: durability — data written by one Database instance must be
// readable by the next one opening the same file.
#include "database.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

int64_t Scalar(Database& db, const std::string& sql) { return db.Execute(sql).back().rows.at(0).at(0).AsInt(); }

TEST(Persistence, TablesRowsAndIndexesSurviveRestart) {
    TempFile f;
    {
        Database db(f.path(), 32);
        db.Execute("CREATE TABLE t (id INT PRIMARY KEY, grp INT, s TEXT); CREATE INDEX t_grp ON t (grp)");
        db.Populate("t", 3000);
        db.Execute("DELETE FROM t WHERE id > 2000; UPDATE t SET s = 'changed' WHERE id = 7");
    }
    Database db(f.path(), 32);
    EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM t"), 2000);
    EXPECT_EQ(db.Execute("SELECT s FROM t WHERE id = 7").back().rows[0][0].AsText(), "changed");
    EXPECT_EQ(db.GetCatalog().RequireTable("t")->row_count, 2000);
    ASSERT_EQ(db.GetCatalog().IndexesOf("t").size(), 2u);
    for (IndexInfo* idx : db.GetCatalog().IndexesOf("t")) EXPECT_EQ(idx->tree->CheckAndCount(), 2000u) << idx->name;
    EXPECT_THROW(db.Execute("INSERT INTO t VALUES (1, 1, 'dup')"), DbError) << "unique index still enforced";
}

TEST(Persistence, StatementsAreDurableWithoutCleanShutdown) {
    TempFile f;
    auto* db = new Database(f.path(), 32);
    db->Execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (1), (2), (3)");
    // Simulate a crash: copy the file while the first instance is still open (no destructor ran).
    std::string copy = f.path() + ".copy";
    std::filesystem::copy_file(f.path(), copy, std::filesystem::copy_options::overwrite_existing);
    {
        Database recovered(copy, 32);
        EXPECT_EQ(Scalar(recovered, "SELECT COUNT(*) FROM t"), 3);
    }
    std::remove(copy.c_str());
    delete db;
}

TEST(Persistence, DroppedObjectsStayDropped) {
    TempFile f;
    {
        Database db(f.path(), 32);
        db.Execute("CREATE TABLE a (x INT); CREATE TABLE b (x INT); CREATE INDEX b_x ON b (x); DROP TABLE a;"
                   "DROP INDEX b_x");
    }
    Database db(f.path(), 32);
    EXPECT_EQ(db.GetCatalog().GetTable("a"), nullptr);
    EXPECT_NE(db.GetCatalog().GetTable("b"), nullptr);
    EXPECT_TRUE(db.GetCatalog().Indexes().empty());
}

TEST(Persistence, ManyRestartsAccumulateData) {
    TempFile f;
    { Database(f.path(), 16).Execute("CREATE TABLE log (id INT PRIMARY KEY, msg TEXT)"); }
    for (int round = 0; round < 5; ++round) {
        Database db(f.path(), 16);
        db.Populate("log", 200);
    }
    Database db(f.path(), 16);
    EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM log"), 1000);
    EXPECT_EQ(Scalar(db, "SELECT MAX(id) FROM log"), 1000) << "ids continue across restarts";
}

TEST(Persistence, NotADatabaseFile) {
    TempFile f;
    {
        std::FILE* fp = std::fopen(f.path().c_str(), "wb");
        std::string junk(PAGE_SIZE, 'J');
        std::fwrite(junk.data(), 1, junk.size(), fp);
        std::fclose(fp);
    }
    EXPECT_THROW(Database(f.path(), 16), DbError);
}

}  // namespace
}  // namespace minidb
