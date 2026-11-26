// catalog.h - system catalog: which tables and indexes exist, and where they live.
#pragma once

#include <functional>
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

// 64-bit FNV-1a hash of a TEXT value: the B+ tree key of a TEXT index.
int64_t HashTextKey(const std::string& s);

// An index on one or more columns ("multi-column" / composite index).
//
// The B+ tree key of a row is built from its key columns:
//   key  - the leading column. INT: the value itself (supports ranges).
//          TEXT: HashTextKey(value), so it answers only equality lookups, and
//          since hashes can collide every hit is re-checked against the real
//          value (see Catalog::KeyTaken and the planner).
//   key2 - composite indexes only (their tree is "wide"). With exactly two
//          columns it is the second column's key as above (an INT second column
//          keeps its order, so `a = 1 AND b < 5` is a range scan), with three or
//          more a hash of all the columns after the first. A NULL there becomes
//          kNullKey2, which a real value can share, so key2 is always re-checked.
// Rows whose leading column is NULL are not indexed: no lookup the index
// answers (`a = ...`, `a < ...`) can match them, and NULLs never conflict.
struct IndexInfo {
    static constexpr int64_t kNullKey2 = INT64_MIN;
    static constexpr size_t kMaxColumns = 32;

    std::string name;
    std::string table;
    std::vector<std::string> columns;  // key columns, in index order
    std::vector<int> col_idxs;         // their positions in the table's rows
    bool unique = false;
    bool hashed = false;          // the leading column is TEXT (key is a hash)
    bool second_is_text = false;  // the second column (if any) is TEXT
    std::unique_ptr<BPlusTree> tree;

    bool Composite() const { return columns.size() > 1; }
    // key2 is the second column's value itself (two columns, the second INT).
    bool OrderedKey2() const { return columns.size() == 2 && !Key2Hashed(); }
    bool Key2Hashed() const { return columns.size() > 2 || (columns.size() == 2 && second_is_text); }
    // A tree hit is the whole truth (single INT column): nothing to re-check.
    bool Exact() const { return !hashed && !Composite(); }

    // The key column values of a full table row, in index order.
    Row KeyValues(const Row& row) const;
    // Whether a row has an entry in the index (its leading column is not NULL).
    bool Indexes(const Row& row) const { return !row[static_cast<size_t>(col_idxs[0])].IsNull(); }
    // The tree entry for key values (in index order, the first one not NULL).
    IndexKey EntryFor(const Row& key_values, RID rid) const;
    // "a=5" or "(a, b)=(5, 'x')", for error messages.
    std::string Describe(const Row& key_values) const;
};

// The tree key of one column value: an INT itself, a TEXT hashed.
int64_t IndexKeyPart(const Value& v);
// key2 of a composite index from the values of its 2nd..last columns.
int64_t IndexKey2(const Value* rest, size_t n);

// The catalog itself is persisted in a chain of pages starting at page 0.
class Catalog {
public:
    explicit Catalog(BufferPoolManager* bpm);

    TableInfo* CreateTable(const std::string& name, std::vector<Column> cols);
    TableInfo* GetTable(const std::string& name);
    TableInfo* RequireTable(const std::string& name);
    void DropTable(const std::string& name);

    // ALTER TABLE support. Renames only change metadata. RewriteTable gives the
    // table a new column list: every row goes through `convert` into a fresh
    // heap, the indexes are rebuilt (those on columns that no longer exist are
    // dropped), and the old pages are freed. All rows are converted and checked
    // before anything is changed, so a failure leaves the table as it was.
    void RenameTable(const std::string& name, const std::string& new_name);
    void RenameColumn(const std::string& table, const std::string& column, const std::string& new_name);
    void RewriteTable(TableInfo* t, std::vector<Column> cols, const std::function<Row(Row)>& convert);

    // An index on one or more columns (INT or TEXT), built from the existing rows.
    IndexInfo* CreateIndex(const std::string& name, const std::string& table, const std::vector<std::string>& columns,
                           bool unique);
    IndexInfo* CreateIndex(const std::string& name, const std::string& table, const std::string& column, bool unique) {
        return CreateIndex(name, table, std::vector<std::string>{column}, unique);
    }
    void DropIndex(const std::string& name);
    std::vector<IndexInfo*> IndexesOf(const std::string& table);

    // True if some row (other than those `ignore` returns true for) has exactly
    // these key values (in index column order) in the index's columns. Used for
    // UNIQUE checks, so a key with a NULL is never taken (NULLs are distinct).
    // Exact even when the tree key is a hash: candidate rows are compared by value.
    bool KeyTaken(IndexInfo* idx, const Row& key_values, const std::function<bool(RID)>& ignore = nullptr);
    bool KeyTaken(IndexInfo* idx, const Value& v, const std::function<bool(RID)>& ignore = nullptr) {
        return KeyTaken(idx, Row{v}, ignore);
    }

    std::vector<TableInfo*> Tables();
    std::vector<IndexInfo*> Indexes();

    void Save();

private:
    void Load();
    bool IsUninitializedCatalogPage();
    std::string Serialize() const;
    void FreePages(const std::vector<page_id_t>& pages);
    static void ResolveIndexColumns(IndexInfo* idx, const Schema& schema);

    BufferPoolManager* bpm_;
    std::map<std::string, std::unique_ptr<TableInfo>> tables_;
    std::map<std::string, std::unique_ptr<IndexInfo>> indexes_;
};

}  // namespace minidb
