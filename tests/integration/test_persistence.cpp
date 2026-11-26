// Integration tests: durability — data written by one Database instance must be
// readable by the next one opening the same file.
#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>

#include "checksum.h"
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
    // Simulate a crash: copy the file and its log while the first instance is
    // still open (no destructor ran).
    std::string copy = f.path() + ".copy";
    testing::CopyCrashImage(f.path(), copy);
    {
        Database recovered(copy, 32);
        EXPECT_EQ(recovered.Disk().RecoveredCommits(), 3u) << "opening, CREATE TABLE, INSERT";
        EXPECT_EQ(Scalar(recovered, "SELECT COUNT(*) FROM t"), 3);
    }
    std::remove(copy.c_str());
    std::remove((copy + ".wal").c_str());
    delete db;
}

// ---- write-ahead log: crashes and rollbacks ---------------------------------

// A summary of the whole contents of table t (or "-" if there is no such
// table), checking on the way that every index agrees with the table.
std::string Fingerprint(Database& db) {
    TableInfo* t = db.GetCatalog().GetTable("t");
    if (!t) return "-";
    Row r = db.Execute("SELECT COUNT(*), SUM(id), SUM(g), SUM(LENGTH(s)) FROM t").back().rows.at(0);
    std::string fp;
    for (const Value& v : r) fp += v.ToString() + "|";
    for (IndexInfo* idx : db.GetCatalog().IndexesOf("t")) {
        size_t n = idx->tree->CheckAndCount();
        EXPECT_EQ(static_cast<int64_t>(n), r[0].AsInt()) << "index " << idx->name << " disagrees with the table";
        fp += idx->name + "|";
    }
    return fp;
}

// The log is cut at every record boundary (and at many points in between), as a
// crash could leave it. Every such database must open, hold exactly the state
// after its last complete commit, and have consistent indexes. With a tiny
// buffer pool, the log also contains pages evicted in the middle of statements.
TEST(Persistence, CrashAtAnyPointOfTheLogRecoversTheLastCommit) {
    TempFile f;
    const DiskOptions no_checkpoints{false, 0};  // everything stays in the log
    std::map<uint64_t, std::string> state_after;  // number of commits -> fingerprint
    std::vector<char> db_bytes, wal_bytes;
    {
        Database db(f.path(), 12, no_checkpoints);
        state_after[0] = state_after[db.Disk().Commits()] = Fingerprint(db);
        auto run = [&](const std::string& sql) {
            db.Execute(sql);
            state_after[db.Disk().Commits()] = Fingerprint(db);
        };
        run("CREATE TABLE t (id INT PRIMARY KEY, g INT, s TEXT)");
        run("CREATE INDEX t_g ON t (g)");
        run("INSERT INTO t VALUES (1, 10, 'one'), (2, 20, 'two'), (3, 30, 'three')");
        db.Populate("t", 600);
        state_after[db.Disk().Commits()] = Fingerprint(db);
        run("UPDATE t SET s = s || '-updated', g = g + 1 WHERE id % 3 = 0");
        run("DELETE FROM t WHERE id > 400");
        run("INSERT INTO t SELECT id + 1000, g, s FROM t WHERE id < 200");
        run("ALTER TABLE t ADD COLUMN extra INT DEFAULT 7");
        run("UPDATE t SET s = 'short' WHERE g < 300");
        EXPECT_EQ(std::filesystem::file_size(f.path()), 0u) << "no checkpoint has happened";
        db_bytes = testing::ReadFileBytes(f.path());
        wal_bytes = testing::ReadFileBytes(f.wal_path());
    }
    ASSERT_GT(state_after.size(), 8u);

    // Record boundaries, found by walking the log.
    std::set<size_t> cuts;
    for (size_t off = WriteAheadLog::kHeaderSize; off <= wal_bytes.size();) {
        cuts.insert(off);
        cuts.insert(off - 1);
        if (off + 4 > wal_bytes.size()) break;
        off += ReadAs<uint32_t>(wal_bytes.data() + off) == 1 ? WriteAheadLog::kPageRecordSize
                                                             : WriteAheadLog::kRecordHeaderSize;
    }
    for (size_t len = 0; len < wal_bytes.size(); len += 1531) cuts.insert(len);
    cuts.insert(wal_bytes.size());

    std::string copy = f.path() + ".crash";
    uint64_t last_recovered = 0;
    for (size_t len : cuts) {
        testing::WriteFileBytes(copy, db_bytes);
        testing::WriteFileBytes(copy + ".wal", wal_bytes.data(), len);
        Database db(copy, 12, no_checkpoints);
        uint64_t k = db.Disk().RecoveredCommits();
        ASSERT_TRUE(state_after.count(k)) << "cut at " << len << ": recovered " << k << " commits";
        EXPECT_EQ(Fingerprint(db), state_after[k]) << "cut at " << len << " (" << k << " commits)";
        EXPECT_GE(k, last_recovered) << "a longer log can only recover more";
        last_recovered = k;
        if (len % 7 == 0 || len == wal_bytes.size()) {  // the recovered database is fully usable
            db.Execute("CREATE TABLE probe (x INT); INSERT INTO probe VALUES (1)");
            EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM probe"), 1);
        }
    }
    EXPECT_EQ(last_recovered, state_after.rbegin()->first) << "the full log recovers every commit";
    std::remove(copy.c_str());
    std::remove((copy + ".wal").c_str());
}

// A statement that fails half-way (here: the disk fails while pages are being
// written, during the statement or its commit) is rolled back completely.
TEST(Persistence, FailedStatementIsRolledBackCompletely) {
    TempFile f;
    std::string before;
    {
        Database db(f.path(), 8);
        db.Execute("CREATE TABLE t (id INT PRIMARY KEY, g INT, s TEXT); CREATE INDEX t_g ON t (g)");
        db.Populate("t", 2000);
        before = Fingerprint(db);
        for (int64_t after : {0, 1, 5, 20}) {
            db.Disk().FailWritesAfter(after);
            EXPECT_THROW(db.Execute("INSERT INTO t SELECT id + 5000, g, s || 'x' FROM t"), DbError)
                << "failing after " << after << " writes";
            db.Disk().FailWritesAfter(-1);
            EXPECT_FALSE(db.Disk().HasUncommittedChanges());
            EXPECT_EQ(Fingerprint(db), before) << "failing after " << after << " writes";
        }
        db.Disk().FailWritesAfter(3);
        EXPECT_THROW(db.Populate("t", 1000), DbError);
        db.Disk().FailWritesAfter(-1);
        EXPECT_EQ(Fingerprint(db), before);

        // The session goes on as if nothing had happened.
        db.Execute("INSERT INTO t SELECT id + 5000, g, s || 'x' FROM t");
        EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM t"), 4000);
        before = Fingerprint(db);
    }
    Database db(f.path(), 8);
    EXPECT_EQ(Fingerprint(db), before);
}

TEST(Persistence, CleanShutdownLeavesNoLogBehind) {
    TempFile f;
    {
        Database db(f.path(), 16);
        db.Execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (1)");
        EXPECT_TRUE(std::filesystem::exists(f.wal_path()));
    }
    EXPECT_FALSE(std::filesystem::exists(f.wal_path()));
    Database db(f.path(), 16);
    EXPECT_EQ(db.Disk().RecoveredCommits(), 0u);
    EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM t"), 1);
}

TEST(Persistence, CheckpointMovesTheLogIntoTheDatabaseFile) {
    TempFile f;
    Database db(f.path(), 16, DiskOptions{true, 0});
    db.Execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (1), (2)");
    EXPECT_GT(db.Disk().Log().PageRecords(), 0u);
    db.Checkpoint();
    EXPECT_EQ(db.Disk().Log().PageRecords(), 0u);
    EXPECT_EQ(std::filesystem::file_size(f.path()), static_cast<uintmax_t>(db.Disk().NumPages()) * DISK_PAGE_SIZE);
    // The database file alone (without the log) is now complete.
    std::string copy = f.path() + ".alone";
    std::filesystem::copy_file(f.path(), copy, std::filesystem::copy_options::overwrite_existing);
    {
        Database alone(copy, 16);
        EXPECT_EQ(Scalar(alone, "SELECT SUM(a) FROM t"), 3);
    }
    std::remove(copy.c_str());
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
// The damaged pages are given fresh, valid checksums. Otherwise the checksum
// would reject every one of them on read, and the structural validation in the
// page layers (still needed for pages a bug wrote wrongly) would never run.
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
    ASSERT_EQ(pristine.size() % DISK_PAGE_SIZE, 0u);
    const size_t pages = pristine.size() / DISK_PAGE_SIZE;
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
            bytes[page * DISK_PAGE_SIZE + rng() % 96] = static_cast<char>(rng() % 256);
        }
        for (size_t page = 0; page < pages; ++page) {
            char* frame = bytes.data() + page * DISK_PAGE_SIZE;
            WriteAs<uint32_t>(frame + PAGE_SIZE, PageChecksum(static_cast<page_id_t>(page), frame));
        }
        {
            std::ofstream out(f.path(), std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        // A log left by the previous round belongs to the file just replaced.
        std::remove(f.wal_path().c_str());
        try {
            // No fsync: this test is about damaged pages, not durability (and
            // opens the database 120 times).
            Database db(f.path(), 16, DiskOptions{/*sync=*/false});
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

// Damage that no structural check can see: one character of a stored value is
// changed on disk. Without checksums the query would silently return the wrong
// value; with them the damage is reported. (Opening the database already walks
// every table's page chain, so the error may come from opening, not the query.)
TEST(Persistence, ChecksumCatchesSilentDataCorruption) {
    TempFile f;
    {
        Database db(f.path(), 32);
        db.Execute("CREATE TABLE accounts (id INT PRIMARY KEY, s TEXT)");
        db.Execute("INSERT INTO accounts VALUES (1, 'balance=1000')");
    }
    std::vector<char> bytes;
    {
        std::ifstream in(f.path(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    const std::string needle = "balance=1000";
    auto it = std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end());
    ASSERT_NE(it, bytes.end());
    *(it + 8) = '9';  // balance=9000
    {
        std::ofstream out(f.path(), std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    try {
        Database db(f.path(), 32);
        auto rows = db.Execute("SELECT s FROM accounts WHERE id = 1").back().rows;
        FAIL() << "the damaged row was returned: " << rows.at(0).at(0).AsText();
    } catch (const DbError& e) {
        EXPECT_NE(std::string(e.what()).find("failed its checksum"), std::string::npos) << e.what();
    }
}

TEST(Persistence, CompositeKeysSurviveRestart) {
    TempFile f;
    {
        Database db(f.path(), 32);
        db.Execute("CREATE TABLE ol (o INT, l INT, item TEXT, PRIMARY KEY (o, l), UNIQUE (item, o))");
        db.Execute("CREATE INDEX ol_three ON ol (l, item, o)");
        db.Populate("ol", 2000);
        db.Execute("INSERT INTO ol VALUES (-1, 1, 'fixed'), (-1, 2, 'other')");
    }
    Database db(f.path(), 32);
    ASSERT_EQ(db.GetCatalog().IndexesOf("ol").size(), 3u);
    for (IndexInfo* idx : db.GetCatalog().IndexesOf("ol")) {
        EXPECT_TRUE(idx->Composite()) << idx->name;
        EXPECT_EQ(idx->tree->CheckAndCount(), 2002u) << idx->name;
    }
    EXPECT_THROW(db.Execute("INSERT INTO ol VALUES (-1, 1, 'new')"), DbError) << "primary key still enforced";
    EXPECT_THROW(db.Execute("INSERT INTO ol VALUES (-1, 3, 'fixed')"), DbError) << "UNIQUE (item, o) too";
    EXPECT_EQ(db.Execute("SELECT item FROM ol WHERE o = -1 AND l = 2").back().rows.at(0).at(0).AsText(), "other");
    EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM ol WHERE l = 1 AND item = 'fixed' AND o = -1"), 1);
}

TEST(Persistence, TextKeysSurviveRestart) {
    TempFile f;
    {
        Database db(f.path(), 32);
        db.Execute("CREATE TABLE u (name TEXT PRIMARY KEY, n INT)");
        db.Execute("INSERT INTO u VALUES ('alice', 1), ('bob', 2)");
    }
    Database db(f.path(), 32);
    EXPECT_THROW(db.Execute("INSERT INTO u VALUES ('alice', 3)"), DbError) << "unique TEXT key still enforced";
    EXPECT_EQ(Scalar(db, "SELECT n FROM u WHERE name = 'bob'"), 2);
}

}  // namespace
}  // namespace minidb
