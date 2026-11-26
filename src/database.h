// database.h - top-level facade: owns all layers and executes SQL statements.
#pragma once

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

class Database {
public:
    explicit Database(const std::string& path, size_t pool_frames = 256);
    ~Database();

    // Executes one or more ';'-separated statements; returns one result per statement.
    std::vector<QueryResult> Execute(const std::string& sql);

    // Fills a table with `n` rows of synthetic data (for experimenting with the planner).
    QueryResult Populate(const std::string& table, int64_t n, uint32_t seed = 42);

    void Checkpoint();  // persist catalog + flush all dirty pages

    Catalog& GetCatalog() { return *catalog_; }
    BufferPoolManager& Bpm() { return *bpm_; }
    DiskManager& Disk() { return *disk_; }

private:
    QueryResult Run(const Statement& stmt);
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

    // Write path. Each modifying statement is atomic: all constraints (types,
    // NOT NULL, row size, uniqueness) are checked for every affected row before
    // the first change is applied, so a failing statement changes nothing.
    Row PrepareRow(const TableInfo* t, Row row) const;
    void InsertRows(TableInfo* t, const std::vector<Row>& rows);

    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<Planner> planner_;
};

}  // namespace minidb
