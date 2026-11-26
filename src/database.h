// database.h - top-level facade: owns all layers and executes SQL statements.
#pragma once

#include <functional>
#include <memory>
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
class Database {
public:
    // Opening a database recovers its write-ahead log first.
    explicit Database(const std::string& path, size_t pool_frames = 256, DiskOptions disk_options = {});
    // Clean shutdown: checkpoints and removes the log. A transaction that is
    // still open is rolled back, not committed.
    ~Database();

    // Executes one or more ';'-separated statements; returns one result per statement.
    std::vector<QueryResult> Execute(const std::string& sql);

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

}  // namespace minidb
