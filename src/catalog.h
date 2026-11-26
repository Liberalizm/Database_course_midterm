// catalog.h - system catalog: which tables and indexes exist, and where they live.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "bplus_tree.h"
#include "table_heap.h"
#include "value.h"

namespace minidb {

struct TableInfo {
    std::string name;
    Schema schema;
    page_id_t first_page = INVALID_PAGE_ID;
    int64_t row_count = 0;  // statistics used by the planner
    std::unique_ptr<TableHeap> heap;
};

struct IndexInfo {
    std::string name;
    std::string table;
    std::string column;
    int col_idx = -1;
    bool unique = false;
    std::unique_ptr<BPlusTree> tree;
};

// The catalog itself is persisted in a chain of pages starting at page 0.
class Catalog {
public:
    explicit Catalog(BufferPoolManager* bpm);

    TableInfo* CreateTable(const std::string& name, std::vector<Column> cols);
    TableInfo* GetTable(const std::string& name);
    TableInfo* RequireTable(const std::string& name);
    void DropTable(const std::string& name);

    IndexInfo* CreateIndex(const std::string& name, const std::string& table, const std::string& column, bool unique);
    void DropIndex(const std::string& name);
    std::vector<IndexInfo*> IndexesOf(const std::string& table);

    std::vector<TableInfo*> Tables();
    std::vector<IndexInfo*> Indexes();

    void Save();

private:
    void Load();
    std::string Serialize() const;
    void FreePages(const std::vector<page_id_t>& pages);

    BufferPoolManager* bpm_;
    std::map<std::string, std::unique_ptr<TableInfo>> tables_;
    std::map<std::string, std::unique_ptr<IndexInfo>> indexes_;
};

}  // namespace minidb
