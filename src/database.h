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
#include "transaction.h"

namespace minidb {

struct QueryResult {
    std::vector<std::string> columns;  // empty for statements that return no rows
    std::vector<Row> rows;
    std::string message;               // e.g. "INSERT 3"
};

class Database {
public:
    // Opening the file rolls back a transaction that a crash interrupted (see
    // transaction.h). `synchronous` = false skips every fsync: faster, and still
    // safe if the process crashes, but a power loss may then damage the file.
    explicit Database(const std::string& path, size_t pool_frames = 256, bool synchronous = true);
    // A transaction that is still open is rolled back, not committed.
    ~Database();

    // Executes one or more ';'-separated statements; returns one result per statement.
    std::vector<QueryResult> Execute(const std::string& sql);

    // Fills a table with `n` rows of synthetic data (for experimenting with the planner).
    // Inside a transaction it becomes part of it, like any other statement.
    QueryResult Populate(const std::string& table, int64_t n, uint32_t seed = 42);

    void Checkpoint();  // persist catalog + flush all dirty pages

    // Transaction state (BEGIN ... COMMIT / ROLLBACK).
    bool InTransaction() const { return in_txn_; }
    const std::vector<std::string>& Savepoints() const { return savepoints_; }
    // True if opening the file rolled back a transaction interrupted by a crash.
    bool RecoveredOnOpen() const { return txn_->RecoveredOnOpen(); }

    Catalog& GetCatalog() { return *catalog_; }
    BufferPoolManager& Bpm() { return *bpm_; }
    DiskManager& Disk() { return *disk_; }
    TransactionManager& Txn() { return *txn_; }

private:
    QueryResult Run(const Statement& stmt);
    QueryResult RunTransaction(const TransactionStmt& s);

    // Every statement that changes the database runs through here, so that it
    // takes effect completely or not at all. Outside a transaction it is a
    // transaction of its own (committed on success). Inside one it runs under a
    // savepoint, so a failure undoes just this statement and the transaction
    // goes on.
    QueryResult Modify(const std::function<QueryResult()>& body);
    // After the file was rolled back (by `rollback_file`): drop the stale buffer
    // pool and reload the catalog. If that fails, the database is unusable
    // until it is reopened (the journal is still there, so reopening recovers).
    void Undo(const std::function<void()>& rollback_file);
    void FlushEverything();  // catalog + dirty pages to the file
    void EndTransaction();
    void CheckUsable() const;
    size_t FindSavepoint(const std::string& name) const;
    QueryResult PopulateImpl(const std::string& table, int64_t n, uint32_t seed);
    QueryResult RunCreateTable(const CreateTableStmt& s);
    QueryResult RunCreateIndex(const CreateIndexStmt& s);
    QueryResult RunDrop(const DropStmt& s);
    QueryResult RunAlterTable(const AlterTableStmt& s);
    std::optional<Value> EvalDefault(const ColumnDef& d);
    QueryResult RunInsert(const InsertStmt& s);
    QueryResult RunSelect(const SelectStmt& s);
    QueryResult RunDelete(const DeleteStmt& s);
    QueryResult RunUpdate(const UpdateStmt& s);
    QueryResult RunExplain(const ExplainStmt& s);

    // Write path. Each modifying statement is atomic: all constraints (types,
    // NOT NULL, row size, uniqueness) are checked for every affected row before
    // the first change is applied, so a failing statement changes nothing.
    Row PrepareRow(const TableInfo* t, Row row) const;
    void InsertRows(TableInfo* t, const std::vector<Row>& rows);

    // Declaration order matters: the transaction manager must recover the file
    // before the buffer pool and catalog read it, and must outlive the buffer
    // pool, whose destructor may still write pages.
    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<TransactionManager> txn_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<Planner> planner_;

    bool in_txn_ = false;                  // inside BEGIN ... COMMIT / ROLLBACK
    std::vector<std::string> savepoints_;  // open SAVEPOINTs, outermost first
    std::string broken_;                   // set if a rollback failed: why
};

}  // namespace minidb
