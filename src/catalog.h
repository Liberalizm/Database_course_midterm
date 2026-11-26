// catalog.h - system catalog: which tables and indexes exist, and where they live.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "bplus_tree.h"
#include "foreign_key.h"
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

// 64-bit FNV-1a hash of a TEXT value: the B+ tree key of a TEXT index.
int64_t HashTextKey(const std::string& s);

struct IndexInfo {
    std::string name;
    std::string table;
    std::string column;
    int col_idx = -1;
    bool unique = false;
    // INT column: the B+ tree key is the value itself (supports ranges).
    // TEXT column: the key is HashTextKey(value), so the index answers only
    // equality lookups, and since hashes can collide every hit is re-checked
    // against the real value (see Catalog::KeyTaken and the planner).
    bool hashed = false;
    std::unique_ptr<BPlusTree> tree;

    int64_t KeyOf(const Value& v) const { return hashed ? HashTextKey(v.AsText()) : v.AsInt(); }
};

// The catalog itself is persisted in a chain of pages starting at page 0.
class Catalog {
public:
    explicit Catalog(BufferPoolManager* bpm);

    TableInfo* CreateTable(const std::string& name, std::vector<Column> cols);
    TableInfo* GetTable(const std::string& name);
    TableInfo* RequireTable(const std::string& name);
    // Refused while a foreign key of another table references this one; the
    // table's own foreign keys are dropped with it.
    void DropTable(const std::string& name);

    // ALTER TABLE support. Renames only change metadata. RewriteTable gives the
    // table a new column list: every row goes through `convert` into a fresh
    // heap, the indexes are rebuilt (those on columns that no longer exist are
    // dropped), and the old pages are freed. All rows are converted and checked
    // before anything is changed, so a failure leaves the table as it was.
    void RenameTable(const std::string& name, const std::string& new_name);
    void RenameColumn(const std::string& table, const std::string& column, const std::string& new_name);
    void RewriteTable(TableInfo* t, std::vector<Column> cols, const std::function<Row(Row)>& convert);

    IndexInfo* CreateIndex(const std::string& name, const std::string& table, const std::string& column, bool unique);
    // Refused for the last unique index of a column that a foreign key references.
    void DropIndex(const std::string& name);
    std::vector<IndexInfo*> IndexesOf(const std::string& table);
    // The PRIMARY KEY index of a table (named "<table>_pkey"), or null.
    IndexInfo* PrimaryKeyIndex(const std::string& table);
    // A unique index on table(column) (the primary key preferred), or null.
    IndexInfo* UniqueIndexOn(const std::string& table, const std::string& column);
    // Any index on table(column) (a unique one preferred), or null.
    IndexInfo* IndexOn(const std::string& table, const std::string& column);

    // Foreign keys. ResolveForeignKey checks the referenced side of `fk` for a
    // referencing column of type `child_type` (the table must exist, the column
    // must carry a unique index, the types must match) and fills in ref_column
    // when it was left out (the primary key). CreateForeignKey also checks that
    // every existing row satisfies the constraint.
    void ResolveForeignKey(ForeignKey* fk, TypeId child_type);
    ForeignKey* CreateForeignKey(ForeignKey fk);
    void DropForeignKey(const std::string& name);
    ForeignKey* GetForeignKey(const std::string& name);
    std::vector<ForeignKey*> ForeignKeys();
    std::vector<ForeignKey*> ForeignKeysOf(const std::string& table);  // `table` is the referencing side
    std::vector<ForeignKey*> ForeignKeysTo(const std::string& table);  // `table` is the referenced side
    // "<table>_<column>_fkey", with a number appended if that name is taken.
    std::string NewForeignKeyName(const std::string& table, const std::string& column);

    // True if some row (other than those `ignore` returns true for) has the
    // non-NULL value `v` in the index's column. Used for UNIQUE checks; exact
    // even for hashed TEXT indexes, whose candidate rows are compared by value.
    bool KeyTaken(IndexInfo* idx, const Value& v, const std::function<bool(RID)>& ignore = nullptr);

    std::vector<TableInfo*> Tables();
    std::vector<IndexInfo*> Indexes();

    void Save();

private:
    void Load();
    bool IsUninitializedCatalogPage();
    std::string Serialize() const;
    void FreePages(const std::vector<page_id_t>& pages);
    void DropIndexUnchecked(const std::string& name);

    BufferPoolManager* bpm_;
    std::map<std::string, std::unique_ptr<TableInfo>> tables_;
    std::map<std::string, std::unique_ptr<IndexInfo>> indexes_;
    std::map<std::string, std::unique_ptr<ForeignKey>> foreign_keys_;
};

}  // namespace minidb
