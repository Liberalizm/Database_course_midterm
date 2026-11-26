// Integration tests: transactions through SQL - BEGIN / COMMIT / ROLLBACK,
// savepoints, statement atomicity, and recovery from crashes at every write.
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <utility>

#include "database.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

int64_t Scalar(Database& db, const std::string& sql) { return db.Execute(sql).back().rows.at(0).at(0).AsInt(); }

// Everything observable about the database, as text: every table's schema,
// statistics and rows (sorted), and every index with its entry count (which
// also checks the B+ tree's structure).
std::string Snapshot(Database& db) {
    std::ostringstream out;
    for (TableInfo* t : db.GetCatalog().Tables()) {
        out << "table " << t->name << " rows=" << t->row_count << " (";
        std::string cols;
        for (size_t i = 0; i < t->schema.Size(); ++i) {
            const Column& c = t->schema.At(i);
            out << c.name << " " << TypeName(c.type) << (c.not_null ? " not null" : "")
                << (c.default_value ? " default " + c.default_value->ToString() : "") << ", ";
            cols += (i ? ", " : "") + c.name;
        }
        out << ")\n";
        // (Keep the result alive: a range-for over a member of a temporary dangles.)
        const auto result = db.Execute("SELECT " + cols + " FROM " + t->name + " ORDER BY " + cols);
        for (const Row& r : result.back().rows) {
            for (const Value& v : r) out << (v.IsNull() ? "NULL" : v.ToString()) << "|";
            out << "\n";
        }
    }
    for (IndexInfo* i : db.GetCatalog().Indexes())
        out << "index " << i->name << " on " << i->table << "(" << i->column << ")" << (i->unique ? " unique" : "")
            << " entries=" << i->tree->CheckAndCount() << "\n";
    return out.str();
}

void ExpectError(Database& db, const std::string& sql, const std::string& fragment) {
    try {
        db.Execute(sql);
        ADD_FAILURE() << "expected an error from: " << sql;
    } catch (const DbError& e) {
        EXPECT_NE(std::string(e.what()).find(fragment), std::string::npos) << sql << "\n  -> " << e.what();
    }
}

std::string ReadBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void WriteBytes(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// What a crash would leave behind at this moment: the database file and the
// journal (if there is one). Every write reaches the OS immediately, so reading
// the files from here sees exactly that.
struct CrashImage {
    std::string db, journal;
    bool has_journal = false;

    static CrashImage Take(const std::string& path) {
        CrashImage img;
        img.db = ReadBytes(path);
        std::string j = TransactionManager::JournalPath(path);
        img.has_journal = std::filesystem::exists(j);
        if (img.has_journal) img.journal = ReadBytes(j);
        return img;
    }
    void RestoreTo(const std::string& path) const {
        WriteBytes(path, db);
        std::string j = TransactionManager::JournalPath(path);
        if (has_journal)
            WriteBytes(j, journal);
        else
            std::remove(j.c_str());
    }
};

// Records a crash image right before every page write of the database.
class CrashRecorder {
public:
    explicit CrashRecorder(Database& db) : db_(db) {
        db_.Disk().SetBeforeWriteHook([this](page_id_t) { images_.push_back(CrashImage::Take(db_.Disk().Path())); });
    }
    ~CrashRecorder() { db_.Disk().SetBeforeWriteHook(nullptr); }
    const std::vector<CrashImage>& Images() const { return images_; }

private:
    Database& db_;
    std::vector<CrashImage> images_;
};

// Opens every crash image and checks that recovery brings back `expected`.
void ExpectEveryCrashRecoversTo(const std::vector<CrashImage>& images, const std::string& expected) {
    TempFile scratch;
    for (size_t k = 0; k < images.size(); ++k) {
        images[k].RestoreTo(scratch.path());
        Database db(scratch.path(), 16);
        ASSERT_EQ(Snapshot(db), expected) << "crash before write #" << k << " of " << images.size();
        EXPECT_FALSE(std::filesystem::exists(TransactionManager::JournalPath(scratch.path())));
    }
}

// A table with a unique and a non-unique index, spread over many pages.
void MakeItems(Database& db, int64_t rows) {
    db.Execute("CREATE TABLE items (id INT PRIMARY KEY, grp INT, label TEXT NOT NULL DEFAULT 'x')");
    db.Execute("CREATE INDEX items_grp ON items (grp)");
    db.Populate("items", rows);
}

// ---- BEGIN / COMMIT / ROLLBACK ----

TEST(Transactions, CommittedChangesAreVisibleAndDurable) {
    TempFile f;
    {
        Database db(f.path(), 32);
        db.Execute("BEGIN");
        EXPECT_TRUE(db.InTransaction());
        db.Execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
        db.Execute("INSERT INTO t VALUES (1, 'one'), (2, 'two'), (3, 'three')");
        db.Execute("UPDATE t SET v = 'TWO' WHERE id = 2");
        db.Execute("DELETE FROM t WHERE id = 3");
        auto r = db.Execute("COMMIT");
        EXPECT_EQ(r.back().message, "COMMIT");
        EXPECT_FALSE(db.InTransaction());
    }
    EXPECT_FALSE(std::filesystem::exists(TransactionManager::JournalPath(f.path())));
    Database db(f.path(), 32);
    EXPECT_FALSE(db.RecoveredOnOpen());
    EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM t"), 2);
    EXPECT_EQ(db.Execute("SELECT v FROM t WHERE id = 2").back().rows[0][0].AsText(), "TWO");
}

TEST(Transactions, ChangesAreVisibleInsideTheTransactionBeforeCommit) {
    TempFile f;
    Database db(f.path(), 32);
    db.Execute("CREATE TABLE t (a INT)");
    db.Execute("BEGIN; INSERT INTO t VALUES (1), (2)");
    EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM t"), 2);
    db.Execute("ROLLBACK");
    EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM t"), 0);
}

TEST(Transactions, RollbackUndoesDataChanges) {
    TempFile f;
    Database db(f.path(), 16);
    MakeItems(db, 2000);
    const std::string before = Snapshot(db);
    const page_id_t pages = db.Disk().NumPages();

    db.Execute("BEGIN");
    db.Execute("INSERT INTO items VALUES (5001, 1, 'new'), (5002, 2, 'newer')");
    db.Execute("UPDATE items SET label = label || ' (a much longer label that moves rows)', grp = grp + 7 WHERE id % 3 = 0");
    db.Execute("DELETE FROM items WHERE id BETWEEN 100 AND 900");
    db.Populate("items", 3000);
    EXPECT_GT(db.Disk().NumPages(), pages);
    EXPECT_NE(Snapshot(db), before);
    EXPECT_EQ(db.Execute("ROLLBACK").back().message, "ROLLBACK");

    EXPECT_FALSE(db.InTransaction());
    EXPECT_EQ(Snapshot(db), before);
    EXPECT_EQ(db.Disk().NumPages(), pages) << "the file shrinks back to its size at BEGIN";
    EXPECT_EQ(db.Bpm().PinnedFrames(), 0u);
}

TEST(Transactions, RollbackUndoesSchemaChanges) {
    TempFile f;
    {
        Database db(f.path(), 32);
        db.Execute("CREATE TABLE keep (id INT PRIMARY KEY, name TEXT UNIQUE)");
        db.Execute("CREATE TABLE doomed (x INT); CREATE INDEX doomed_x ON doomed (x)");
        db.Execute("INSERT INTO keep VALUES (1, 'a'), (2, 'b'); INSERT INTO doomed VALUES (10), (20)");
        const std::string before = Snapshot(db);

        db.Execute("BEGIN");
        db.Execute("CREATE TABLE fresh (y INT PRIMARY KEY); INSERT INTO fresh VALUES (1)");
        db.Execute("DROP TABLE doomed");
        db.Execute("CREATE INDEX keep_name2 ON keep (name)");
        db.Execute("ALTER TABLE keep ADD COLUMN score INT DEFAULT 5");
        db.Execute("ALTER TABLE keep RENAME COLUMN name TO title");
        db.Execute("ALTER TABLE keep RENAME TO kept");
        EXPECT_EQ(db.GetCatalog().GetTable("keep"), nullptr);
        db.Execute("ROLLBACK");

        EXPECT_EQ(Snapshot(db), before);
        EXPECT_EQ(db.GetCatalog().GetTable("fresh"), nullptr);
        EXPECT_EQ(Scalar(db, "SELECT SUM(x) FROM doomed WHERE x > 0"), 30);
        ExpectError(db, "INSERT INTO keep VALUES (3, 'a')", "duplicate key");  // the unique index is back
    }
    Database db(f.path(), 32);
    EXPECT_EQ(db.GetCatalog().GetTable("fresh"), nullptr);
    EXPECT_EQ(Scalar(db, "SELECT COUNT(*) FROM doomed"), 2);
}

TEST(Transactions, DroppedTableComesBackWithItsData) {
    TempFile f;
    Database db(f.path(), 16);
    MakeItems(db, 1500);
    const std::string before = Snapshot(db);
    db.Execute("BEGIN; DROP TABLE items");
    db.Execute("CREATE TABLE other (a INT)");
    db.Populate("other", 2000);  // reuses the freed pages of "items"
    db.Execute("ROLLBACK");
    EXPECT_EQ(Snapshot(db), before);
}

TEST(Transactions, OpenTransactionIsRolledBackWhenTheDatabaseIsClosed) {
    TempFile f;
    {
        Database db(f.path(), 32);
        db.Execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (1)");
        db.Execute("BEGIN; INSERT INTO t VALUES (2); DELETE FROM t WHERE a = 1");
    }
    Database db(f.path(), 32);
    EXPECT_FALSE(db.RecoveredOnOpen()) << "rolled back on close, not by recovery";
    EXPECT_EQ(Scalar(db, "SELECT SUM(a) FROM t"), 1);
}

TEST(Transactions, SeveralTransactionsInARow) {
    TempFile f;
    Database db(f.path(), 32);
    db.Execute("CREATE TABLE t (a INT)");
    for (int i = 0; i < 10; ++i) {
        db.Execute("BEGIN; INSERT INTO t VALUES (" + std::to_string(i) + ")");
        db.Execute(i % 2 ? "COMMIT" : "ROLLBACK");
    }
    EXPECT_EQ(Scalar(db, "SELECT SUM(a) FROM t"), 1 + 3 + 5 + 7 + 9);
}

TEST(Transactions, SynonymsWork) {
    TempFile f;
    Database db(f.path(), 32);
    db.Execute("CREATE TABLE t (a INT)");
    db.Execute("BEGIN TRANSACTION; INSERT INTO t VALUES (1); COMMIT WORK");
    db.Execute("START TRANSACTION; INSERT INTO t VALUES (2); END");
    db.Execute("BEGIN WORK; INSERT INTO t VALUES (100); ABORT");
    db.Execute("BEGIN; INSERT INTO t VALUES (200); ROLLBACK TRANSACTION");
    EXPECT_EQ(Scalar(db, "SELECT SUM(a) FROM t"), 3);
}

TEST(Transactions, MisplacedTransactionStatementsAreErrors) {
    TempFile f;
    Database db(f.path(), 32);
    ExpectError(db, "COMMIT", "there is no transaction in progress");
    ExpectError(db, "ROLLBACK", "there is no transaction in progress");
    ExpectError(db, "SAVEPOINT s", "there is no transaction in progress");
    ExpectError(db, "RELEASE s", "there is no transaction in progress");
    ExpectError(db, "ROLLBACK TO s", "there is no transaction in progress");
    db.Execute("BEGIN");
    ExpectError(db, "BEGIN", "already a transaction in progress");
    EXPECT_TRUE(db.InTransaction()) << "the error does not end the transaction";
    ExpectError(db, "RELEASE nope", "savepoint \"nope\" does not exist");
    ExpectError(db, "ROLLBACK TO nope", "savepoint \"nope\" does not exist");
    db.Execute("COMMIT");
}

// ---- statement atomicity ----

TEST(Transactions, FailingStatementOnlyUndoesItselfAndTheTransactionGoesOn) {
    TempFile f;
    Database db(f.path(), 32);
    db.Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    db.Execute("BEGIN; INSERT INTO t VALUES (1, 10)");
    ExpectError(db, "INSERT INTO t VALUES (2, 20), (1, 99)", "duplicate key");
    ExpectError(db, "UPDATE t SET v = v / 0", "division by zero");
    ExpectError(db, "SELECT * FROM missing", "does not exist");
    EXPECT_TRUE(db.InTransaction());
    db.Execute("INSERT INTO t VALUES (3, 30)");
    db.Execute("COMMIT");
    EXPECT_EQ(Scalar(db, "SELECT SUM(id) FROM t"), 4);
    EXPECT_EQ(Scalar(db, "SELECT SUM(v) FROM t"), 40);
}

// An I/O error in the middle of a statement - after some of its pages have
// already been written - must still leave no trace of the statement.
void FailHalfwayThroughAStatement(bool in_transaction) {
    TempFile f;
    Database db(f.path(), 16);
    MakeItems(db, 1500);
    if (in_transaction) db.Execute("BEGIN; DELETE FROM items WHERE id <= 100");
    db.Checkpoint();  // a clean pool: every write from now on comes from the UPDATE
    const std::string before = Snapshot(db);

    int writes = 0;
    db.Disk().SetBeforeWriteHook([&](page_id_t) {
        if (++writes == 25) throw DbError("simulated I/O error");
    });
    ExpectError(db, "UPDATE items SET label = label || ' with a much longer label than before', grp = grp + 1",
                "simulated I/O error");
    db.Disk().SetBeforeWriteHook(nullptr);
    ASSERT_GE(writes, 25);

    EXPECT_EQ(db.InTransaction(), in_transaction);
    EXPECT_EQ(Snapshot(db), before);
    if (in_transaction) db.Execute("COMMIT");
    db.Execute("UPDATE items SET grp = grp + 1 WHERE id = 500");  // still fully usable
    db.Execute("UPDATE items SET grp = grp - 1 WHERE id = 500");
    EXPECT_EQ(Snapshot(db), before);
}

TEST(Transactions, IoErrorHalfwayThroughAStatementInsideATransaction) { FailHalfwayThroughAStatement(true); }
TEST(Transactions, IoErrorHalfwayThroughAnAutocommitStatement) { FailHalfwayThroughAStatement(false); }

TEST(Transactions, FailedCommitRollsBackTheTransaction) {
    TempFile f;
    Database db(f.path(), 32);
    db.Execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (1)");
    db.Execute("BEGIN; INSERT INTO t VALUES (2)");
    bool failed = false;  // only the first write fails (the rollback's writes must work)
    db.Disk().SetBeforeWriteHook([&](page_id_t) {
        if (!std::exchange(failed, true)) throw DbError("disk full");
    });
    ExpectError(db, "COMMIT", "COMMIT failed, the transaction was rolled back: disk full");
    db.Disk().SetBeforeWriteHook(nullptr);
    EXPECT_FALSE(db.InTransaction());
    EXPECT_EQ(Scalar(db, "SELECT SUM(a) FROM t"), 1);
}

TEST(Transactions, FailedRollbackLeavesTheDatabaseUnusableUntilReopened) {
    TempFile f;
    std::string before;
    {
        Database db(f.path(), 16);
        MakeItems(db, 1500);
        before = Snapshot(db);
        db.Execute("BEGIN; UPDATE items SET grp = grp + 1; DELETE FROM items WHERE id > 700");
        db.Checkpoint();  // the changes are in the file now, the originals in the journal
        db.Disk().SetBeforeWriteHook([](page_id_t) { throw DbError("device gone"); });
        ExpectError(db, "ROLLBACK", "device gone");
        db.Disk().SetBeforeWriteHook(nullptr);
        ExpectError(db, "SELECT COUNT(*) FROM items", "reopen it");
        EXPECT_THROW(db.Populate("items", 1), DbError);
    }
    Database db(f.path(), 16);
    EXPECT_TRUE(db.RecoveredOnOpen());
    EXPECT_EQ(Snapshot(db), before);
}

// ---- savepoints ----

std::string Ids(Database& db) {
    std::string out;
    const auto result = db.Execute("SELECT id FROM t ORDER BY id");
    for (const Row& r : result.back().rows) out += r[0].ToString() + " ";
    return out;
}

TEST(Savepoints, RollbackToAndRelease) {
    TempFile f;
    Database db(f.path(), 32);
    db.Execute("CREATE TABLE t (id INT PRIMARY KEY)");
    db.Execute("BEGIN; INSERT INTO t VALUES (1)");
    EXPECT_EQ(db.Execute("SAVEPOINT a").back().message, "SAVEPOINT");
    db.Execute("INSERT INTO t VALUES (2); SAVEPOINT b; INSERT INTO t VALUES (3)");
    EXPECT_EQ(db.Savepoints(), (std::vector<std::string>{"a", "b"}));

    EXPECT_EQ(db.Execute("ROLLBACK TO b").back().message, "ROLLBACK");
    EXPECT_EQ(Ids(db), "1 2 ");
    db.Execute("INSERT INTO t VALUES (4)");
    db.Execute("ROLLBACK TO SAVEPOINT a");
    EXPECT_EQ(Ids(db), "1 ");
    EXPECT_EQ(db.Savepoints(), (std::vector<std::string>{"a"})) << "b was destroyed, a stays";
    ExpectError(db, "ROLLBACK TO b", "savepoint \"b\" does not exist");

    db.Execute("INSERT INTO t VALUES (5)");
    EXPECT_EQ(db.Execute("RELEASE SAVEPOINT a").back().message, "RELEASE");
    EXPECT_TRUE(db.Savepoints().empty());
    db.Execute("COMMIT");
    EXPECT_EQ(Ids(db), "1 5 ");
}

TEST(Savepoints, ReleaseKeepsChangesUntilTheTransactionEnds) {
    TempFile f;
    Database db(f.path(), 32);
    db.Execute("CREATE TABLE t (id INT PRIMARY KEY)");
    db.Execute("BEGIN; SAVEPOINT a; INSERT INTO t VALUES (1); SAVEPOINT b; INSERT INTO t VALUES (2)");
    db.Execute("RELEASE a");  // releases b too
    EXPECT_TRUE(db.Savepoints().empty());
    EXPECT_EQ(Ids(db), "1 2 ");
    db.Execute("ROLLBACK");
    EXPECT_EQ(Ids(db), "");
}

TEST(Savepoints, ChangesOfAReleasedSavepointRollBackWithTheEnclosingOne) {
    TempFile f;
    Database db(f.path(), 16);
    MakeItems(db, 1500);
    db.Execute("BEGIN; SAVEPOINT outer_sp");
    const std::string at_outer = Snapshot(db);
    db.Execute("UPDATE items SET grp = 0 WHERE id < 500");
    db.Execute("SAVEPOINT inner_sp");
    db.Execute("UPDATE items SET grp = -1, label = 'moved ' || label WHERE id >= 400");
    db.Populate("items", 500);
    db.Execute("RELEASE inner_sp");
    db.Execute("ROLLBACK TO outer_sp");
    EXPECT_EQ(Snapshot(db), at_outer);
    db.Execute("COMMIT");
    EXPECT_EQ(Snapshot(db), at_outer);
}

TEST(Savepoints, NamesCanBeReused) {
    TempFile f;
    Database db(f.path(), 32);
    db.Execute("CREATE TABLE t (id INT PRIMARY KEY)");
    db.Execute("BEGIN; SAVEPOINT s; INSERT INTO t VALUES (1); SAVEPOINT s; INSERT INTO t VALUES (2)");
    db.Execute("ROLLBACK TO s");  // the most recent "s"
    EXPECT_EQ(Ids(db), "1 ");
    db.Execute("RELEASE s");  // releases the second "s"
    db.Execute("ROLLBACK TO s");  // now the first one
    EXPECT_EQ(Ids(db), "");
    db.Execute("COMMIT");
}

TEST(Savepoints, SchemaChangesRollBackToASavepoint) {
    TempFile f;
    Database db(f.path(), 32);
    db.Execute("CREATE TABLE t (id INT PRIMARY KEY); INSERT INTO t VALUES (1)");
    db.Execute("BEGIN; CREATE TABLE u (x INT); SAVEPOINT s");
    db.Execute("DROP TABLE t; ALTER TABLE u ADD COLUMN y TEXT; CREATE INDEX u_x ON u (x)");
    db.Execute("ROLLBACK TO s");
    ASSERT_NE(db.GetCatalog().GetTable("t"), nullptr);
    ASSERT_NE(db.GetCatalog().GetTable("u"), nullptr);
    EXPECT_EQ(db.GetCatalog().GetTable("u")->schema.Size(), 1u);
    EXPECT_TRUE(db.GetCatalog().IndexesOf("u").empty());
    db.Execute("COMMIT");
    EXPECT_EQ(Ids(db), "1 ");
}

// ---- crashes ----

TEST(Crash, InterruptedTransactionIsRolledBackOnOpen) {
    TempFile f;
    auto* db = new Database(f.path(), 16);
    MakeItems(*db, 1500);
    const std::string before = Snapshot(*db);
    db->Execute("BEGIN; UPDATE items SET grp = grp * 2; DELETE FROM items WHERE id < 300");
    db->Populate("items", 2000);  // grows the file; evictions write changed pages
    CrashImage crash = CrashImage::Take(f.path());
    ASSERT_TRUE(crash.has_journal);
    delete db;

    TempFile copy;
    crash.RestoreTo(copy.path());
    Database recovered(copy.path(), 16);
    EXPECT_TRUE(recovered.RecoveredOnOpen());
    EXPECT_EQ(Snapshot(recovered), before);
}

TEST(Crash, AtEveryWriteOfATransactionTheOldStateSurvives) {
    TempFile f;
    Database db(f.path(), 16);
    MakeItems(db, 800);
    const std::string before = Snapshot(db);
    std::vector<CrashImage> images;
    {
        CrashRecorder rec(db);
        db.Execute("BEGIN");
        db.Execute("UPDATE items SET label = label || '!', grp = grp + 1 WHERE id % 2 = 0");
        db.Execute("DELETE FROM items WHERE id BETWEEN 200 AND 400");
        db.Execute("CREATE TABLE log (id INT PRIMARY KEY, what TEXT)");
        db.Execute("INSERT INTO log SELECT id, label FROM items WHERE id < 150");
        db.Execute("COMMIT");
        images = rec.Images();
    }
    const std::string after = Snapshot(db);
    ASSERT_GT(images.size(), 20u);
    // Every write happens before the commit point (deleting the journal).
    ExpectEveryCrashRecoversTo(images, before);
    ExpectEveryCrashRecoversTo({CrashImage::Take(f.path())}, after);
}

TEST(Crash, AtEveryWriteOfAnAutocommitStatementTheOldStateSurvives) {
    TempFile f;
    Database db(f.path(), 16);
    MakeItems(db, 800);
    const std::string before = Snapshot(db);
    std::vector<CrashImage> images;
    {
        CrashRecorder rec(db);
        db.Execute("UPDATE items SET label = label || ' and some more text', grp = -grp");
        images = rec.Images();
    }
    ASSERT_GT(images.size(), 10u);
    ExpectEveryCrashRecoversTo(images, before);
}

TEST(Crash, AtEveryWriteOfARollbackTheOldStateSurvives) {
    TempFile f;
    Database db(f.path(), 16);
    MakeItems(db, 800);
    const std::string before = Snapshot(db);
    db.Execute("BEGIN; UPDATE items SET grp = 0; DELETE FROM items WHERE id > 600");
    db.Checkpoint();  // the changes are in the file, so ROLLBACK has pages to write back
    std::vector<CrashImage> images;
    {
        CrashRecorder rec(db);
        db.Execute("ROLLBACK");
        images = rec.Images();
    }
    ASSERT_GT(images.size(), 5u);
    ExpectEveryCrashRecoversTo(images, before);
    EXPECT_EQ(Snapshot(db), before);
}

TEST(Crash, CommittedTransactionSurvivesACrashRightAfterCommit) {
    TempFile f;
    auto* db = new Database(f.path(), 16);
    db->Execute("CREATE TABLE t (a INT); BEGIN; INSERT INTO t VALUES (1), (2); COMMIT");
    CrashImage crash = CrashImage::Take(f.path());
    EXPECT_FALSE(crash.has_journal);
    delete db;
    TempFile copy;
    crash.RestoreTo(copy.path());
    Database recovered(copy.path(), 16);
    EXPECT_FALSE(recovered.RecoveredOnOpen());
    EXPECT_EQ(Scalar(recovered, "SELECT SUM(a) FROM t"), 3);
}

// ---- randomized: SQL vs. an in-memory model ----

// Random statements, transactions, savepoints, failing statements and restarts
// on a tiny buffer pool (so pages are evicted - written - in mid-statement).
// After every step the table must match a std::map that applies the same
// operations with the expected transaction semantics.
TEST(Transactions, RandomizedAgainstAModel) {
    using Model = std::map<int64_t, std::pair<int64_t, std::string>>;
    for (uint32_t seed = 1; seed <= 3; ++seed) {
        TempFile f;
        auto db = std::make_unique<Database>(f.path(), 12, /*synchronous=*/false);
        db->Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT, s TEXT); CREATE INDEX t_v ON t (v)");
        std::mt19937 rng(seed);
        auto rnd = [&](int64_t lo, int64_t hi) { return lo + static_cast<int64_t>(rng() % static_cast<uint32_t>(hi - lo + 1)); };

        Model committed, current;
        bool in_txn = false;
        std::vector<std::pair<std::string, Model>> savepoints;
        int sp_counter = 0;

        auto check = [&](const std::string& when) {
            std::string expected, actual;
            for (const auto& [id, vs] : current)
                expected += std::to_string(id) + "," + std::to_string(vs.first) + "," + vs.second + ";";
            const auto result = db->Execute("SELECT id, v, s FROM t ORDER BY id");
            for (const Row& r : result.back().rows)
                actual += r[0].ToString() + "," + r[1].ToString() + "," + r[2].ToString() + ";";
            ASSERT_EQ(actual, expected) << "seed " << seed << ", " << when;
            for (IndexInfo* idx : db->GetCatalog().IndexesOf("t"))
                ASSERT_EQ(idx->tree->CheckAndCount(), current.size()) << idx->name << ", " << when;
            ASSERT_EQ(db->GetCatalog().RequireTable("t")->row_count, static_cast<int64_t>(current.size()));
        };

        for (int step = 0; step < 400; ++step) {
            std::string sql;
            bool fails = false;
            Model next = current;
            const int64_t a = rnd(1, 300), b = a + rnd(0, 40);
            switch (rng() % 12) {
                case 0:
                case 1: {  // insert one or two rows; a duplicate key fails the whole statement
                    int64_t id1 = rnd(1, 300), id2 = rnd(1, 300);
                    bool two = rng() % 2;
                    std::string pad(static_cast<size_t>(rnd(10, 300)), 'p');
                    sql = "INSERT INTO t VALUES (" + std::to_string(id1) + ", " + std::to_string(id1 % 17) + ", '" + pad +
                          "')";
                    fails = next.count(id1) > 0;
                    next[id1] = {id1 % 17, pad};
                    if (two) {
                        sql += ", (" + std::to_string(id2) + ", 5, 'x')";
                        fails = fails || next.count(id2) > 0;
                        next[id2] = {5, "x"};
                    }
                    break;
                }
                case 2: {  // grow / shrink rows (moves them between pages)
                    std::string pad(static_cast<size_t>(rnd(0, 200)), 'g');
                    sql = "UPDATE t SET s = '" + pad + "', v = v + 1 WHERE id BETWEEN " + std::to_string(a) + " AND " +
                          std::to_string(b);
                    for (auto& [id, vs] : next)
                        if (id >= a && id <= b) vs = {vs.first + 1, pad};
                    break;
                }
                case 3: {  // change keys; fails if a new key is taken
                    sql = "UPDATE t SET id = id + 1000 WHERE id BETWEEN " + std::to_string(a) + " AND " + std::to_string(b);
                    Model moved;
                    for (auto& [id, vs] : current) {
                        int64_t nid = id >= a && id <= b ? id + 1000 : id;
                        if (id >= a && id <= b && current.count(nid)) fails = true;
                        moved[nid] = vs;
                    }
                    next = moved;
                    break;
                }
                case 4:
                    sql = "DELETE FROM t WHERE id BETWEEN " + std::to_string(a) + " AND " + std::to_string(b);
                    for (auto it = next.begin(); it != next.end();)
                        it = it->first >= a && it->first <= b ? next.erase(it) : std::next(it);
                    break;
                case 5:  // fails as soon as it reaches a row
                    sql = "UPDATE t SET v = v / 0 WHERE id BETWEEN " + std::to_string(a) + " AND " + std::to_string(b);
                    for (auto& [id, vs] : current) fails = fails || (id >= a && id <= b);
                    break;
                case 6:
                case 7:
                    if (!in_txn) {
                        sql = "BEGIN";
                        in_txn = true;
                    } else {
                        sql = "SAVEPOINT sp" + std::to_string(++sp_counter);
                        savepoints.emplace_back("sp" + std::to_string(sp_counter), current);
                    }
                    break;
                case 8:
                    if (!savepoints.empty()) {  // ROLLBACK TO a random savepoint
                        size_t i = rng() % savepoints.size();
                        sql = "ROLLBACK TO " + savepoints[i].first;
                        next = savepoints[i].second;
                        savepoints.resize(i + 1);
                    } else if (in_txn) {
                        sql = "COMMIT";
                        committed = current;
                        in_txn = false;
                    }
                    break;
                case 9:
                    if (!savepoints.empty()) {  // RELEASE a random savepoint
                        size_t i = rng() % savepoints.size();
                        sql = "RELEASE " + savepoints[i].first;
                        savepoints.resize(i);
                    }
                    break;
                case 10:
                    if (in_txn) {
                        sql = rng() % 2 ? "COMMIT" : "ROLLBACK";
                        if (sql == "COMMIT")
                            committed = current;
                        else
                            next = committed;
                        in_txn = false;
                        savepoints.clear();
                    }
                    break;
                case 11:
                    if (rng() % 4 == 0) {  // restart: an open transaction is rolled back
                        db.reset();
                        db = std::make_unique<Database>(f.path(), 12, /*synchronous=*/false);
                        if (in_txn) current = committed;
                        in_txn = false;
                        savepoints.clear();
                        check("after restart, step " + std::to_string(step));
                    }
                    break;
            }
            if (sql.empty()) continue;
            try {
                db->Execute(sql);
                ASSERT_FALSE(fails) << "seed " << seed << " step " << step << ": should have failed: " << sql;
                current = next;
            } catch (const DbError& e) {
                ASSERT_TRUE(fails) << "seed " << seed << " step " << step << ": " << sql << "\n  -> " << e.what();
            }
            if (!in_txn) committed = current;
            ASSERT_EQ(db->InTransaction(), in_txn) << sql;
            if (step % 10 == 0) check("step " + std::to_string(step) + " after " + sql);
            if (HasFatalFailure()) return;
        }
        if (in_txn) db->Execute("COMMIT");
        committed = current;
        db.reset();
        db = std::make_unique<Database>(f.path(), 12, /*synchronous=*/false);
        check("after the final restart");
    }
}

}  // namespace
}  // namespace minidb
