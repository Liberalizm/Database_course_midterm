// Integration tests: concurrency control. Exclusive file locking between
// processes, and Connections sharing one Database from several threads.
#include <atomic>
#include <random>
#include <thread>

#include "database.h"
#include "test_util.h"

namespace minidb {
namespace {

using namespace std::chrono_literals;
using testing::TempFile;

class ConcurrencyTest : public ::testing::Test {
protected:
    TempFile file;
    std::unique_ptr<Database> db = std::make_unique<Database>(file.path(), 64);

    static int64_t Count(Connection& c, const std::string& table) {
        return c.Execute("SELECT COUNT(*) FROM " + table).back().rows.at(0).at(0).AsInt();
    }
    static std::string ErrorOf(const std::function<void()>& f) {
        try {
            f();
        } catch (const DbError& e) {
            return e.what();
        }
        return "";
    }
};

TEST_F(ConcurrencyTest, DatabaseFileIsLockedWhileOpen) {
    db->Execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (1)");
    std::string err = ErrorOf([&] { Database second(file.path(), 16); });
    EXPECT_NE(err.find("locked"), std::string::npos) << err;
    // The lock does not stop plain reads of the file (backups, crash images).
    db->Checkpoint();  // committed data moves from the log into the file
    EXPECT_FALSE(testing::ReadFileBytes(file.path()).empty());
    db.reset();
    Database reopened(file.path(), 16);  // free again once closed
    EXPECT_EQ(reopened.Execute("SELECT a FROM t").back().rows.size(), 1u);
}

TEST_F(ConcurrencyTest, AFailedOpenLeavesTheLockWithItsOwner) {
    EXPECT_THROW(Database(file.path(), 16), DbError);
    EXPECT_THROW(Database(file.path(), 16), DbError) << "still held by the first Database";
    db->Execute("CREATE TABLE t (a INT)");  // and the owner keeps working
}

TEST_F(ConcurrencyTest, ConnectionsShareCommittedData) {
    Connection a(db.get()), b(db.get());
    a.Execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (1), (2)");
    EXPECT_EQ(Count(b, "t"), 2);
    b.Execute("INSERT INTO t VALUES (3)");
    EXPECT_EQ(Count(a, "t"), 3);
    EXPECT_EQ(db->Execute("SELECT COUNT(*) FROM t").back().rows[0][0].AsInt(), 3) << "the default connection too";
}

TEST_F(ConcurrencyTest, AnOpenTransactionLocksOutOtherConnections) {
    db->SetBusyTimeout(50ms);
    Connection a(db.get()), b(db.get());
    a.Execute("CREATE TABLE t (a INT); BEGIN; INSERT INTO t VALUES (1)");
    EXPECT_TRUE(a.InTransaction());
    EXPECT_FALSE(b.InTransaction());
    std::string err = ErrorOf([&] { b.Execute("SELECT * FROM t"); });
    EXPECT_NE(err.find("database is locked"), std::string::npos) << "no dirty reads: " << err;
    EXPECT_NE(ErrorOf([&] { b.Execute("INSERT INTO t VALUES (2)"); }), "");
    EXPECT_NE(ErrorOf([&] { db->Execute("SELECT 1"); }), "") << "the default connection waits as well";
    EXPECT_EQ(Count(a, "t"), 1) << "the owner reads its own changes";
    a.Execute("COMMIT");
    EXPECT_FALSE(a.InTransaction());
    EXPECT_EQ(Count(b, "t"), 1);
}

TEST_F(ConcurrencyTest, AWaitingConnectionProceedsWhenTheTransactionEnds) {
    Connection a(db.get()), b(db.get());
    a.Execute("CREATE TABLE t (a INT); BEGIN; INSERT INTO t VALUES (1)");
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        b.Execute("INSERT INTO t VALUES (2)");  // blocks until a commits
        done = true;
    });
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(done) << "must wait for a's transaction";
    a.Execute("INSERT INTO t VALUES (3); COMMIT");
    waiter.join();
    EXPECT_TRUE(done);
    EXPECT_EQ(Count(a, "t"), 3);
}

TEST_F(ConcurrencyTest, RollbackAndFailuresReleaseOrKeepTheLockCorrectly) {
    db->SetBusyTimeout(50ms);
    Connection a(db.get()), b(db.get());
    a.Execute("CREATE TABLE t (a INT PRIMARY KEY); INSERT INTO t VALUES (1)");
    a.Execute("BEGIN; INSERT INTO t VALUES (2)");
    EXPECT_THROW(a.Execute("INSERT INTO t VALUES (1)"), DbError);  // fails, transaction goes on
    EXPECT_TRUE(a.InTransaction());
    EXPECT_THROW(b.Execute("SELECT 1"), DbError) << "a still holds the database";
    a.Execute("ROLLBACK");
    EXPECT_EQ(Count(b, "t"), 1);
    EXPECT_THROW(b.Execute("SELEC 1"), DbError);  // a parse error takes no lock
    EXPECT_EQ(Count(a, "t"), 1);
}

TEST_F(ConcurrencyTest, ClosingAConnectionRollsBackItsTransaction) {
    db->SetBusyTimeout(50ms);
    db->Execute("CREATE TABLE t (a INT)");
    {
        Connection a(db.get());
        a.Execute("BEGIN; INSERT INTO t VALUES (1), (2)");
    }
    Connection b(db.get());
    EXPECT_EQ(Count(b, "t"), 0) << "rolled back, and the database is free again";
    b.Execute("INSERT INTO t VALUES (3)");
    db.reset();  // committed data survives a reopen
    db = std::make_unique<Database>(file.path(), 64);
    EXPECT_EQ(db->Execute("SELECT COUNT(*) FROM t").back().rows[0][0].AsInt(), 1);
}

// Many threads move money between accounts in transactions (some rolled back,
// some failing on a constraint) while others read. Transactions are
// serializable, so every reader sees the same total and none of them is lost.
TEST_F(ConcurrencyTest, ConcurrentTransfersKeepTheTotal) {
    db->SetBusyTimeout(60s);
    constexpr int kAccounts = 20, kThreads = 6, kTransfers = 60;
    db->Execute("CREATE TABLE acct (id INT PRIMARY KEY, balance INT); CREATE TABLE log (id INT, amount INT)");
    for (int i = 0; i < kAccounts; ++i) db->Execute("INSERT INTO acct VALUES (" + std::to_string(i) + ", 1000)");

    std::atomic<int> committed{0}, failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            Connection c(db.get());
            std::mt19937 rng(static_cast<uint32_t>(t + 1));
            for (int i = 0; i < kTransfers; ++i) {
                const int from = static_cast<int>(rng() % kAccounts), to = static_cast<int>(rng() % kAccounts);
                const int amount = static_cast<int>(rng() % 50) + 1;
                const std::string move = "BEGIN; UPDATE acct SET balance = balance - " + std::to_string(amount) +
                                         " WHERE id = " + std::to_string(from) +
                                         "; UPDATE acct SET balance = balance + " + std::to_string(amount) +
                                         " WHERE id = " + std::to_string(to) + "; INSERT INTO log VALUES (" +
                                         std::to_string(t) + ", " + std::to_string(amount) + ")";
                try {
                    switch (i % 5) {
                        case 0:  // changed its mind
                            c.Execute(move + "; ROLLBACK");
                            break;
                        case 1:  // a statement fails inside the transaction, which still commits
                            c.Execute(move);
                            EXPECT_THROW(c.Execute("INSERT INTO acct VALUES (0, 1)"), DbError);
                            c.Execute("COMMIT");
                            ++committed;
                            break;
                        default:
                            c.Execute(move + "; COMMIT");
                            ++committed;
                    }
                    // A reader in the same thread: the total never changes.
                    auto total = c.Execute("SELECT SUM(balance) FROM acct").back().rows.at(0).at(0).AsInt();
                    if (total != kAccounts * 1000) ++failures;
                } catch (const DbError& e) {
                    ADD_FAILURE() << "thread " << t << ": " << e.what();
                    ++failures;
                    c.Execute("ROLLBACK");  // best effort
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(failures, 0);
    EXPECT_EQ(committed, kThreads * kTransfers * 4 / 5);
    auto r = db->Execute("SELECT SUM(balance), (SELECT COUNT(*) FROM log) FROM acct").back().rows.at(0);
    EXPECT_EQ(r[0].AsInt(), kAccounts * 1000);
    EXPECT_EQ(r[1].AsInt(), committed.load());
    EXPECT_FALSE(db->InTransaction());
}

}  // namespace
}  // namespace minidb
