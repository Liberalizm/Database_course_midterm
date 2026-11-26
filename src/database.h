// database.h - top-level facade: owns all layers and executes SQL statements.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "buffer_pool.h"
#include "catalog.h"
#include "disk_manager.h"
#include "parser.h"
#include "planner.h"

namespace minidb {

struct QueryResult {
    std::vector<std::string> columns;  // empty for statements that return no rows
    std::vector<Row> rows;
    std::string message;               // e.g. "INSERT 3"
};

// Transactions sit on the write-ahead log (see wal.h): one transaction is one
// commit group of the log.
//
//   autocommit  every modifying statement outside BEGIN ... COMMIT is a
//               transaction of its own: committed if it succeeds, rolled back
//               if anything fails.
//   BEGIN       the statements that follow share one commit group.
//   COMMIT      the catalog and all dirty pages go to the log, then one COMMIT
//               record and an fsync: the transaction is durable as a whole.
//   ROLLBACK    the log cuts the group off, the buffer pool is discarded and
//               the catalog reloaded from the last committed state.
//   SAVEPOINT   a mark inside the group (after flushing every change made so
//               far into it); ROLLBACK TO cuts the log back to the mark.
//   crash       recovery keeps only complete commit groups, so an unfinished
//               transaction leaves no trace.
//
// Inside a transaction every statement also runs under a savepoint of its own,
// so a failing statement is undone without ending the transaction.
//
// Concurrency. The database file is locked while it is open, so another
// process (or a second Database on the same file) is refused instead of
// corrupting it. Within a process, any number of Connections (see below) can
// share one Database from any threads. Their statements run one at a time,
// and an open transaction holds the whole database until it commits or rolls
// back: other connections wait for it (up to the busy timeout, then their
// statement fails with "database is locked"). Locking the whole database for
// the duration of a transaction makes every schedule serializable, and no
// connection ever sees another's uncommitted changes. Database::Execute itself
// runs on a built-in default connection.
class Connection;

class Database {
public:
    // Opening a database recovers its write-ahead log first.
    explicit Database(const std::string& path, size_t pool_frames = 256, DiskOptions disk_options = {});
    // Clean shutdown: checkpoints and removes the log. A transaction that is
    // still open is rolled back, not committed.
    ~Database();

    // Executes one or more ';'-separated statements; returns one result per statement.
    std::vector<QueryResult> Execute(const std::string& sql);

    // How long a statement waits for another connection's transaction to end
    // before it fails with "database is locked" (default 5 s).
    void SetBusyTimeout(std::chrono::milliseconds t);

    // Fills a table with `n` rows of synthetic data (for experimenting with the planner).
    // Inside a transaction it becomes part of it, like any other statement.
    QueryResult Populate(const std::string& table, int64_t n, uint32_t seed = 42);

    // Copies all committed pages from the write-ahead log into the database file
    // and empties the log. (Also happens automatically as the log grows.) Not
    // allowed inside a transaction.
    void Checkpoint();

    // Transaction state (BEGIN ... COMMIT / ROLLBACK).
    bool InTransaction() const { return in_txn_; }
    const std::vector<std::string>& Savepoints() const { return savepoints_; }

    Catalog& GetCatalog() { return *catalog_; }
    BufferPoolManager& Bpm() { return *bpm_; }
    DiskManager& Disk() { return *disk_; }

private:
    friend class Connection;

    // Exclusive use of the database by one session (a Connection, or the default
    // session of Execute / Populate / Checkpoint) for the guard's lifetime. While
    // that session has a transaction open it keeps the database after the guard
    // is gone. Throws "database is locked" after the busy timeout.
    class SessionGuard {
    public:
        SessionGuard(Database* db, const void* session);
        ~SessionGuard();
        SessionGuard(const SessionGuard&) = delete;
        SessionGuard& operator=(const SessionGuard&) = delete;

    private:
        Database* db_;
        const void* session_;
    };
    std::vector<QueryResult> ExecuteAs(const void* session, const std::string& sql);
    // A connection goes away: its open transaction (if any) is rolled back.
    void CloseSession(const void* session);

    std::mutex mu_;  // guards owner_ and depth_
    std::condition_variable released_;
    const void* owner_ = nullptr;  // the session using the database, if any
    int depth_ = 0;                // nested guards of the owner
    std::chrono::milliseconds busy_timeout_{5000};
    const char default_session_ = 0;

    QueryResult Run(const Statement& stmt);
    QueryResult RunTransaction(const TransactionStmt& s);
    QueryResult RunCreateTable(const CreateTableStmt& s);
    QueryResult RunCreateIndex(const CreateIndexStmt& s);
    QueryResult RunCreateView(const CreateViewStmt& s);
    QueryResult RunDrop(const DropStmt& s);
    QueryResult RunAlterTable(const AlterTableStmt& s);
    std::optional<Value> EvalDefault(const ColumnDef& d);
    QueryResult RunInsert(const InsertStmt& s);
    QueryResult RunSelect(const SelectStmt& s);
    QueryResult RunDelete(const DeleteStmt& s);
    QueryResult RunUpdate(const UpdateStmt& s);
    QueryResult RunExplain(const ExplainStmt& s);
    void PopulateRows(const std::string& table, int64_t n, uint32_t seed);

    // Every statement that changes the database runs through here, so that it
    // takes effect completely or not at all: outside a transaction it is
    // committed on success; inside one it runs under a savepoint.
    QueryResult Modify(const std::function<QueryResult()>& body);
    void FlushToLog();  // catalog + dirty pages into the log (not committed)
    void Commit();      // FlushToLog, then a COMMIT record and fsync
    // After a failure: drop the stale buffer pool, let `rollback_log` cut the
    // log back, and reload the catalog. If that fails, the database is unusable
    // until it is reopened (recovery then keeps only what was committed).
    void Undo(const std::function<void()>& rollback_log);
    void EndTransaction();
    void CheckUsable() const;
    size_t FindSavepoint(const std::string& name) const;

    // Each modifying statement is atomic: its changes are collected in a WriteSet
    // (write_set.h), which runs the foreign key actions and checks every
    // constraint before the first change is applied.
    void InsertRows(TableInfo* t, const std::vector<Row>& rows);
    // Completes a parsed foreign key of `table` (name) and creates it.
    void AddForeignKey(const std::string& table, ForeignKey fk);

    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<Planner> planner_;

    bool in_txn_ = false;                  // inside BEGIN ... COMMIT / ROLLBACK
    std::vector<std::string> savepoints_;  // open SAVEPOINTs, outermost first
    std::string broken_;                   // set if a rollback failed: why
};

// One user of a shared Database: its statements and transactions are its own.
// Destroying a connection rolls back its open transaction. A Connection must
// not be used by two threads at once, and must not outlive its Database.
class Connection {
public:
    explicit Connection(Database* db) : db_(db) {}
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    std::vector<QueryResult> Execute(const std::string& sql) { return db_->ExecuteAs(this, sql); }
    // Whether this connection has a transaction open (BEGIN without COMMIT).
    bool InTransaction();

private:
    Database* db_;
};

}  // namespace minidb
