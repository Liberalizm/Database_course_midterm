// write_set.h - the write path shared by INSERT, UPDATE and DELETE.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "catalog.h"

namespace minidb {

// Casts a full row to the column types and enforces NOT NULL.
Row PrepareRow(const TableInfo* t, Row row);

// Every change one modifying statement makes, collected before anything is
// written. Statement atomicity without a log rests on this: Apply()
//   1. runs the foreign key actions (ON DELETE / ON UPDATE CASCADE, SET NULL,
//      SET DEFAULT), which may add changes to further tables, recursively;
//   2. checks every constraint (types, NOT NULL, row size, UNIQUE, FOREIGN KEY)
//      against the state at the *end* of the statement, as the SQL standard
//      requires (so "UPDATE t SET id = id + 1" works although keys collide
//      midway, and a row may reference a row inserted by the same statement);
//   3. and only then writes the heaps and indexes.
// A statement that fails in step 1 or 2 therefore changes nothing.
class WriteSet {
public:
    explicit WriteSet(Catalog* catalog) : catalog_(catalog) {}

    // Rows passed in are full rows of `t`; they are cast and checked for NOT NULL here.
    void Insert(TableInfo* t, Row row);
    void Update(TableInfo* t, RID rid, Row old_row, Row new_row);
    void Delete(TableInfo* t, RID rid, Row old_row);

    void Apply();

private:
    struct Change {                  // an existing row that is updated or deleted
        Row old_row;
        std::optional<Row> new_row;  // nullopt: deleted
        std::string tuple;           // serialized new_row (set by Validate)
        RID new_rid;                 // where new_row ended up (set by Write)
    };
    struct Insertion {
        Row row;
        std::string tuple;
    };
    struct TableChanges {
        TableInfo* table = nullptr;
        std::map<RID, Change> changed;
        std::vector<Insertion> inserted;
        // Column -> encodings of its non-NULL values in all new rows (updated or
        // inserted). Built on first use, after the foreign key actions have run.
        std::map<size_t, std::unordered_set<std::string>> new_keys;
    };
    // How to find the rows that reference a key through one foreign key: by an
    // index on the referencing column, or else by one scan per statement.
    struct ChildLookup {
        bool ready = false;
        IndexInfo* index = nullptr;
        std::unordered_map<std::string, std::vector<RID>> by_key;
    };

    TableChanges& For(TableInfo* t);
    TableChanges* Find(const std::string& table);
    Row ReadRow(TableInfo* t, RID rid);
    // Rows of fk.table that held `key` in fk.column at the start of the statement.
    std::vector<RID> ReferencingRows(const ForeignKey& fk, const Value& key);
    const std::unordered_set<std::string>& NewKeys(TableChanges& tc, size_t col);
    // Does fk.ref_table hold `key` in fk.ref_column at the end of the statement?
    bool ParentHasKey(const ForeignKey& fk, const Value& key);

    void RunForeignKeyActions();
    void Validate();
    void CheckUnique(TableChanges& tc);
    void CheckForeignKeys();
    void Write(TableChanges& tc);

    Catalog* catalog_;
    std::map<std::string, TableChanges> tables_;
    std::map<std::string, ChildLookup> lookups_;  // by foreign key name
};

}  // namespace minidb
