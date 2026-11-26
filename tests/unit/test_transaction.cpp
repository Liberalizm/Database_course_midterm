// Unit tests: TransactionManager (rollback journal, savepoints, crash recovery),
// working directly on a DiskManager - no buffer pool, no catalog.
//
// Pages are filled with a single repeated letter, so the whole file can be
// compared as a short string: "abc" = three pages filled with 'a', 'b' and 'c'.
#include <filesystem>
#include <fstream>
#include <functional>

#include "transaction.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

void Put(DiskManager& d, page_id_t pid, char c) {
    std::string s(PAGE_SIZE, c);
    d.WritePage(pid, s.data());
}

page_id_t Append(DiskManager& d, char c) {
    page_id_t pid = d.AllocatePage();
    Put(d, pid, c);
    return pid;
}

// The file as a string of one letter per page ('?' for a page that is not uniform).
std::string Pages(DiskManager& d) {
    std::string out;
    std::string buf(PAGE_SIZE, '\0');
    for (page_id_t p = 0; p < d.NumPages(); ++p) {
        d.ReadPage(p, buf.data());
        bool uniform = buf.find_first_not_of(buf[0]) == std::string::npos;
        out += uniform ? buf[0] : '?';
    }
    return out;
}

void MakeFile(const std::string& path, const std::string& pages) {
    DiskManager d(path);
    for (char c : pages) Append(d, c);
}

std::string JournalOf(const TempFile& f) { return TransactionManager::JournalPath(f.path()); }
bool JournalExists(const TempFile& f) { return std::filesystem::exists(JournalOf(f)); }

// Size of a journal holding `records` saved pages (header + records).
uintmax_t JournalSize(size_t records) { return 24 + records * (8 + PAGE_SIZE); }

TEST(Transaction, CommitKeepsChangesAndDeletesTheJournal) {
    TempFile f;
    MakeFile(f.path(), "abc");
    DiskManager d(f.path());
    TransactionManager tm(&d);
    tm.Begin();
    Put(d, 1, 'X');
    Append(d, 'Y');
    EXPECT_TRUE(JournalExists(f)) << "the journal exists while the transaction runs";
    tm.Commit();
    EXPECT_FALSE(tm.Active());
    EXPECT_FALSE(JournalExists(f));
    EXPECT_EQ(Pages(d), "aXcY");
}

TEST(Transaction, RollbackRestoresContentsAndFileLength) {
    TempFile f;
    MakeFile(f.path(), "abc");
    DiskManager d(f.path());
    TransactionManager tm(&d);
    tm.Begin();
    Put(d, 0, 'X');
    Put(d, 2, 'Z');
    Put(d, 0, 'W');  // second write of the same page
    Append(d, 'N');
    Append(d, 'M');
    EXPECT_EQ(Pages(d), "WbZNM");
    tm.Rollback();
    EXPECT_EQ(Pages(d), "abc");
    EXPECT_EQ(d.NumPages(), 3) << "pages added by the transaction are gone";
    EXPECT_FALSE(JournalExists(f));
}

TEST(Transaction, EachPageIsJournaledOnlyOnce) {
    TempFile f;
    MakeFile(f.path(), "abc");
    DiskManager d(f.path());
    TransactionManager tm(&d);
    tm.Begin();
    for (char c = 'A'; c <= 'E'; ++c) Put(d, 1, c);
    EXPECT_EQ(tm.JournaledPages(), 1u);
    EXPECT_EQ(std::filesystem::file_size(JournalOf(f)), JournalSize(1));
    tm.Rollback();
    EXPECT_EQ(Pages(d), "abc") << "the image from BEGIN, not from an intermediate write";
}

TEST(Transaction, PagesAddedByTheTransactionAreNotJournaled) {
    TempFile f;
    MakeFile(f.path(), "ab");
    DiskManager d(f.path());
    TransactionManager tm(&d);
    tm.Begin();
    page_id_t p = Append(d, 'n');
    Put(d, p, 'm');
    EXPECT_EQ(tm.JournaledPages(), 0u);
    EXPECT_TRUE(JournalExists(f)) << "growing the file needs the journal (it records the original length)";
    tm.Rollback();
    EXPECT_EQ(Pages(d), "ab");
}

TEST(Transaction, NoJournalWithoutATransactionOrWithoutWrites) {
    TempFile f;
    MakeFile(f.path(), "ab");
    DiskManager d(f.path());
    TransactionManager tm(&d);
    Put(d, 0, 'X');  // no transaction: written in place
    EXPECT_FALSE(JournalExists(f));
    tm.Begin();
    tm.Commit();
    tm.Begin();
    tm.Rollback();
    EXPECT_FALSE(JournalExists(f));
    EXPECT_EQ(Pages(d), "Xb");
}

TEST(Transaction, BeginTwiceOrEndWithoutBeginIsAnError) {
    TempFile f;
    DiskManager d(f.path());
    TransactionManager tm(&d);
    EXPECT_THROW(tm.Commit(), DbError);
    EXPECT_THROW(tm.Rollback(), DbError);
    EXPECT_THROW(tm.Savepoint(), DbError);
    tm.Begin();
    EXPECT_THROW(tm.Begin(), DbError);
    EXPECT_THROW(tm.ReleaseSavepoint(), DbError);
    EXPECT_THROW(tm.RollbackToSavepoint(), DbError);
}

// ---- crash recovery ----

// Leaves a hot journal behind: the transaction manager is destroyed while its
// transaction is still open, exactly as if the process had died.
void CrashMidTransaction(const std::string& path, const std::function<void(DiskManager&)>& writes) {
    DiskManager d(path);
    TransactionManager tm(&d);
    tm.Begin();
    writes(d);
}

TEST(Transaction, HotJournalIsRolledBackWhenTheFileIsOpened) {
    TempFile f;
    MakeFile(f.path(), "abc");
    CrashMidTransaction(f.path(), [](DiskManager& d) {
        Put(d, 1, 'X');
        Append(d, 'N');
    });
    ASSERT_TRUE(JournalExists(f));
    {
        DiskManager raw(f.path());
        EXPECT_EQ(Pages(raw), "aXcN") << "the crash left the file half-changed";
    }
    DiskManager d(f.path());
    TransactionManager tm(&d);
    EXPECT_TRUE(tm.RecoveredOnOpen());
    EXPECT_EQ(Pages(d), "abc");
    EXPECT_FALSE(JournalExists(f));
}

TEST(Transaction, NoRecoveryWithoutAJournal) {
    TempFile f;
    MakeFile(f.path(), "abc");
    DiskManager d(f.path());
    TransactionManager tm(&d);
    EXPECT_FALSE(tm.RecoveredOnOpen());
    EXPECT_EQ(Pages(d), "abc");
}

TEST(Transaction, TornLastJournalRecordIsIgnored) {
    TempFile f;
    MakeFile(f.path(), "abc");
    CrashMidTransaction(f.path(), [](DiskManager& d) {
        Put(d, 0, 'X');
        // Journals page 1 but crashes before writing it (and before syncing the record).
        d.PrepareWrite(1);
    });
    ASSERT_EQ(std::filesystem::file_size(JournalOf(f)), JournalSize(2));
    // The crash tore the second record.
    std::filesystem::resize_file(JournalOf(f), JournalSize(2) - 100);
    DiskManager d(f.path());
    TransactionManager tm(&d);
    EXPECT_TRUE(tm.RecoveredOnOpen());
    EXPECT_EQ(Pages(d), "abc");
}

TEST(Transaction, DamagedLastJournalRecordIsIgnored) {
    TempFile f;
    MakeFile(f.path(), "abc");
    CrashMidTransaction(f.path(), [](DiskManager& d) {
        Put(d, 0, 'X');
        d.PrepareWrite(2);
    });
    {
        std::fstream j(JournalOf(f), std::ios::in | std::ios::out | std::ios::binary);
        j.seekp(static_cast<std::streamoff>(JournalSize(1) + 8 + 100));  // inside record 2's page image
        j.put('#');
    }
    DiskManager d(f.path());
    TransactionManager tm(&d);
    EXPECT_EQ(Pages(d), "abc") << "page 2 must not be 'restored' from a damaged record";
}

TEST(Transaction, JournalWithoutAValidHeaderIsDiscarded) {
    for (const std::string& junk : {std::string(), std::string("junk"), std::string(100, 'J')}) {
        TempFile f;
        MakeFile(f.path(), "abc");
        {
            std::ofstream j(JournalOf(f), std::ios::binary);
            j << junk;
        }
        DiskManager d(f.path());
        TransactionManager tm(&d);
        EXPECT_FALSE(tm.RecoveredOnOpen()) << junk.size() << " bytes of junk";
        EXPECT_EQ(Pages(d), "abc");
        EXPECT_FALSE(JournalExists(f));
    }
}

TEST(Transaction, JournalThatDoesNotMatchTheFileIsRefused) {
    TempFile f;
    MakeFile(f.path(), "abcde");
    CrashMidTransaction(f.path(), [](DiskManager& d) { Put(d, 4, 'X'); });
    std::filesystem::resize_file(f.path(), 2 * PAGE_SIZE);  // someone replaced / cut the file
    DiskManager d(f.path());
    EXPECT_THROW(TransactionManager tm(&d), DbError);
    EXPECT_TRUE(JournalExists(f)) << "the journal is kept for a human to look at";
}

TEST(Transaction, RecoveryWorksWithoutFsyncToo) {
    TempFile f;
    MakeFile(f.path(), "abc");
    {
        DiskManager d(f.path());
        TransactionManager tm(&d, /*synchronous=*/false);
        tm.Begin();
        Put(d, 2, 'X');
        tm.Rollback();
        EXPECT_EQ(Pages(d), "abc");
        tm.Begin();
        Put(d, 0, 'Y');
        Append(d, 'Z');
    }
    DiskManager d(f.path());
    TransactionManager tm(&d, /*synchronous=*/false);
    EXPECT_TRUE(tm.RecoveredOnOpen());
    EXPECT_EQ(Pages(d), "abc");
}

// ---- savepoints ----

TEST(Transaction, RollbackToSavepointRestoresTheStateAtTheSavepoint) {
    TempFile f;
    MakeFile(f.path(), "abc");
    DiskManager d(f.path());
    TransactionManager tm(&d);
    tm.Begin();
    Put(d, 0, 'X');
    tm.Savepoint();
    Put(d, 0, 'Y');
    Put(d, 1, 'Q');
    Append(d, 'N');
    tm.RollbackToSavepoint();
    EXPECT_EQ(Pages(d), "Xbc") << "changes before the savepoint stay";
    EXPECT_EQ(tm.SavepointDepth(), 1u) << "the savepoint itself stays";
    Put(d, 2, 'Z');
    tm.RollbackToSavepoint();  // again, to the same savepoint
    EXPECT_EQ(Pages(d), "Xbc");
    tm.ReleaseSavepoint();
    tm.Commit();
    EXPECT_EQ(Pages(d), "Xbc");
}

TEST(Transaction, ReleasedSavepointsChangesCanStillBeRolledBackByTheEnclosingOne) {
    TempFile f;
    MakeFile(f.path(), "abc");
    DiskManager d(f.path());
    TransactionManager tm(&d);
    tm.Begin();
    tm.Savepoint();  // outer
    Put(d, 0, 'P');  // written in the outer savepoint before the inner one
    tm.Savepoint();  // inner
    Put(d, 0, 'Q');
    Put(d, 1, 'B');  // first written inside the inner savepoint
    tm.ReleaseSavepoint();
    EXPECT_EQ(Pages(d), "QBc");
    tm.RollbackToSavepoint();  // outer
    EXPECT_EQ(Pages(d), "abc") << "page 0 back to its image at the outer savepoint, not the inner one";
    tm.Rollback();
    EXPECT_EQ(Pages(d), "abc");
}

TEST(Transaction, NestedSavepointRollbacks) {
    TempFile f;
    MakeFile(f.path(), "abcd");
    DiskManager d(f.path());
    TransactionManager tm(&d);
    tm.Begin();
    Put(d, 0, 'A');
    tm.Savepoint();
    Put(d, 1, 'B');
    page_id_t n = Append(d, 'n');
    tm.Savepoint();
    Put(d, 2, 'C');
    Put(d, n, 'm');
    Append(d, 'o');
    tm.RollbackToSavepoint();
    EXPECT_EQ(Pages(d), "ABcdn");
    tm.ReleaseSavepoint();
    tm.RollbackToSavepoint();
    EXPECT_EQ(Pages(d), "Abcd");
    tm.ReleaseSavepoint();
    Put(d, 3, 'D');
    tm.Commit();
    EXPECT_EQ(Pages(d), "AbcD");
}

TEST(Transaction, FullRollbackAfterSavepointsUndoesEverything) {
    TempFile f;
    MakeFile(f.path(), "abc");
    DiskManager d(f.path());
    TransactionManager tm(&d);
    tm.Begin();
    Put(d, 0, 'X');
    tm.Savepoint();
    Put(d, 1, 'Y');
    tm.RollbackToSavepoint();
    Put(d, 2, 'Z');
    tm.Savepoint();
    Append(d, 'N');
    tm.Rollback();
    EXPECT_EQ(Pages(d), "abc");
    EXPECT_EQ(tm.SavepointDepth(), 0u);
}

TEST(Transaction, CrashInsideASavepointRollsBackTheWholeTransaction) {
    TempFile f;
    MakeFile(f.path(), "abc");
    {
        DiskManager d(f.path());
        TransactionManager tm(&d);
        tm.Begin();
        Put(d, 0, 'X');
        tm.Savepoint();
        Put(d, 1, 'Y');
        tm.RollbackToSavepoint();
        Put(d, 1, 'Z');
        Append(d, 'N');
    }
    DiskManager d(f.path());
    TransactionManager tm(&d);
    EXPECT_EQ(Pages(d), "abc");
}

}  // namespace
}  // namespace minidb
