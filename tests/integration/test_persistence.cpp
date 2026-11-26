// Integration tests: durability — data written by one Database instance must be
// readable by the next one opening the same file.
#include <fstream>
#include <iterator>
#include <random>

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

TEST(Persistence, DroppedTablesDoNotGrowTheFileForever) {
    TempFile f;
    page_id_t after_first_cycle = 0;
    for (int cycle = 0; cycle < 5; ++cycle) {
        Database db(f.path(), 32);  // a restart between every cycle
        db.Execute("CREATE TABLE scratch (id INT PRIMARY KEY, grp INT, s TEXT); CREATE INDEX scratch_grp ON scratch (grp)");
        db.Populate("scratch", 2000);
        EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM scratch WHERE id > 0"), 2000);
        db.Execute("DROP TABLE scratch");
        if (cycle == 0) after_first_cycle = db.Disk().NumPages();
        EXPECT_EQ(db.Disk().NumPages(), after_first_cycle) << "cycle " << cycle << " should only reuse freed pages";
    }
}

// A damaged database file must produce errors, never crash or hang. Flips bytes in
// the structural parts of pages (headers, slot arrays, node headers) and runs a
// mix of reads and writes; any crash takes down this test process.
TEST(Persistence, CorruptedFileNeverCrashesOrHangs) {
    TempFile f;
    {
        Database db(f.path(), 32);
        db.Execute("CREATE TABLE t (id INT PRIMARY KEY, g INT, s TEXT); CREATE INDEX tg ON t (g)");
        db.Populate("t", 3000);
    }
    std::vector<char> pristine;
    {
        std::ifstream in(f.path(), std::ios::binary);
        pristine.assign(std::istreambuf_iterator<char>(in), {});
    }
    const size_t pages = pristine.size() / PAGE_SIZE;
    ASSERT_GT(pages, 20u);
    const char* queries[] = {"SELECT COUNT(*) FROM t",
                             "SELECT COUNT(*) FROM t WHERE id = 500",
                             "SELECT COUNT(*) FROM t WHERE g = 5",
                             "SELECT MAX(s) FROM t",
                             "UPDATE t SET g = g + 1 WHERE id < 100",
                             "DELETE FROM t WHERE id > 2900",
                             "INSERT INTO t VALUES (99999, 1, 'x')"};
    int errors = 0;
    for (uint32_t seed = 1; seed <= 120; ++seed) {
        std::mt19937 rng(seed);
        std::vector<char> bytes = pristine;
        int flips = 1 + static_cast<int>(rng() % 6);
        for (int k = 0; k < flips; ++k) {
            size_t page = 1 + rng() % (pages - 1);  // page 0 (catalog) is tested elsewhere
            bytes[page * PAGE_SIZE + rng() % 96] = static_cast<char>(rng() % 256);
        }
        {
            std::ofstream out(f.path(), std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        try {
            Database db(f.path(), 16);
            for (const char* q : queries) {
                try {
                    db.Execute(q);
                } catch (const DbError&) {
                    ++errors;
                }
            }
        } catch (const DbError&) {
            ++errors;  // refusing to open a damaged file is fine too
        }
    }
    EXPECT_GT(errors, 0) << "the damage should have been noticed at least sometimes";
}

}  // namespace
}  // namespace minidb
