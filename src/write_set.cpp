#include "write_set.h"

#include <algorithm>
#include <deque>

namespace minidb {

namespace {

std::string Encode(const Value& v) {
    std::string s;
    v.AppendKey(&s);
    return s;
}

// A hashable encoding of index key values (for duplicates within one statement).
std::string Encode(const Row& key) {
    std::string s;
    for (const Value& v : key) v.AppendKey(&s);
    return s;
}

bool SameKey(const Value& a, const Value& b) {
    return a.IsNull() == b.IsNull() && (a.IsNull() || a.Compare(b) == 0);
}

bool SameKey(const Row& a, const Row& b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (!SameKey(a[i], b[i])) return false;
    return true;
}

bool HasNull(const Row& key) {
    return std::any_of(key.begin(), key.end(), [](const Value& v) { return v.IsNull(); });
}

size_t ColumnOf(const TableInfo* t, const std::string& column) {
    int i = t->schema.Find("", column);
    if (i < 0) throw DbError("internal: column \"" + column + "\" of table \"" + t->name + "\" not found");
    return static_cast<size_t>(i);
}

[[noreturn]] void ThrowDuplicate(const IndexInfo* idx, const Row& key) {
    throw DbError("duplicate key value violates unique constraint \"" + idx->name + "\": " + idx->Describe(key));
}

std::string SerializeChecked(const Schema& schema, const Row& row) {
    std::string tuple = SerializeTuple(schema, row);
    if (tuple.size() > TablePage::MAX_TUPLE)
        throw DbError("row is too large (" + std::to_string(tuple.size()) + " bytes, max " +
                      std::to_string(TablePage::MAX_TUPLE) + ")");
    return tuple;
}

}  // namespace

Row PrepareRow(const TableInfo* t, Row row) {
    const Schema& schema = t->schema;
    for (size_t i = 0; i < schema.Size(); ++i) {
        row[i] = row[i].CastTo(schema.At(i).type);
        if (row[i].IsNull() && schema.At(i).not_null)
            throw DbError("null value in column \"" + schema.At(i).name + "\" violates not-null constraint");
    }
    return row;
}

WriteSet::TableChanges& WriteSet::For(TableInfo* t) {
    TableChanges& tc = tables_[t->name];
    tc.table = t;
    return tc;
}

WriteSet::TableChanges* WriteSet::Find(const std::string& table) {
    auto it = tables_.find(table);
    return it == tables_.end() ? nullptr : &it->second;
}

void WriteSet::Insert(TableInfo* t, Row row) { For(t).inserted.push_back({PrepareRow(t, std::move(row)), {}}); }

void WriteSet::Update(TableInfo* t, RID rid, Row old_row, Row new_row) {
    For(t).changed[rid] = Change{std::move(old_row), PrepareRow(t, std::move(new_row)), {}, {}};
}

void WriteSet::Delete(TableInfo* t, RID rid, Row old_row) {
    For(t).changed[rid] = Change{std::move(old_row), std::nullopt, {}, {}};
}

void WriteSet::Apply() {
    RunForeignKeyActions();
    Validate();
    for (auto& [name, tc] : tables_) Write(tc);
}

Row WriteSet::ReadRow(TableInfo* t, RID rid) {
    std::string buf;
    if (!t->heap->Get(rid, &buf)) throw DbError("internal: row of table \"" + t->name + "\" not found");
    return DeserializeTuple(t->schema, buf.data(), buf.size());
}

// Nothing has been written yet while this runs, so heaps and indexes still show
// the state at the start of the statement.
std::vector<RID> WriteSet::ReferencingRows(const ForeignKey& fk, const Value& key) {
    TableInfo* child = catalog_->RequireTable(fk.table);
    const size_t col = ColumnOf(child, fk.column);
    ChildLookup& lk = lookups_[fk.name];
    if (!lk.ready) {
        lk.ready = true;
        lk.index = catalog_->IndexOn(fk.table, fk.column);
        if (!lk.index) {
            auto it = child->heap->Begin();
            RID rid;
            std::string buf;
            while (it.Next(&rid, &buf)) {
                Row row = DeserializeTuple(child->schema, buf.data(), buf.size());
                if (!row[col].IsNull()) lk.by_key[Encode(row[col])].push_back(rid);
            }
        }
    }
    std::vector<RID> out;
    if (lk.index) {
        // The column is the index's only or leading one, so its key is the tree key
        // (key2, if any, can be anything: LowerBound starts below every key2).
        const int64_t k = IndexKeyPart(key);
        for (auto it = lk.index->tree->LowerBound(k); it.Valid() && it.Entry().key == k; it.Next()) {
            RID rid = it.Entry().rid;
            if (lk.index->hashed && !SameKey(ReadRow(child, rid)[col], key)) continue;  // hash collision
            out.push_back(rid);
        }
    } else if (auto f = lk.by_key.find(Encode(key)); f != lk.by_key.end()) {
        out = f->second;
    }
    return out;
}

void WriteSet::RunForeignKeyActions() {
    if (catalog_->ForeignKeys().empty()) return;
    // Every existing row that changes may affect the rows referencing it; rows
    // changed by an action go back on the list, so actions chain through tables.
    std::deque<std::pair<TableChanges*, RID>> work;
    for (auto& [name, tc] : tables_)
        for (auto& [rid, ch] : tc.changed) work.emplace_back(&tc, rid);

    // Each step changes or deletes some row, so this ends, except when ON UPDATE
    // CASCADE constraints form a cycle that keeps passing values around.
    size_t budget = 1024;
    for (TableInfo* t : catalog_->Tables()) budget += 16 * static_cast<size_t>(std::max<int64_t>(t->row_count, 0));

    while (!work.empty()) {
        auto [tc, rid] = work.front();
        work.pop_front();
        if (budget-- == 0)
            throw DbError("foreign key actions do not terminate (a cycle of ON UPDATE CASCADE constraints?)");
        for (ForeignKey* fk : catalog_->ForeignKeysTo(tc->table->name)) {
            const Change& ch = tc->changed.at(rid);
            const size_t pcol = ColumnOf(tc->table, fk->ref_column);
            const Value old_key = ch.old_row[pcol];
            if (old_key.IsNull()) continue;  // nothing references NULL
            const bool deleted = !ch.new_row;
            if (!deleted && SameKey(old_key, (*ch.new_row)[pcol])) continue;
            const FkAction action = deleted ? fk->on_delete : fk->on_update;
            if (action == FkAction::NO_ACTION || action == FkAction::RESTRICT) continue;  // see CheckForeignKeys

            TableInfo* child = catalog_->RequireTable(fk->table);
            const size_t ccol = ColumnOf(child, fk->column);
            const bool delete_child = action == FkAction::CASCADE && deleted;
            Value target;  // the new value of the referencing column (NULL for SET NULL)
            if (action == FkAction::CASCADE && !deleted)
                target = (*ch.new_row)[pcol];
            else if (action == FkAction::SET_DEFAULT)
                target = child->schema.At(ccol).default_value.value_or(Value::Null());

            TableChanges& cc = For(child);
            for (RID crid : ReferencingRows(*fk, old_key)) {
                auto it = cc.changed.find(crid);
                if (it != cc.changed.end() && !it->second.new_row) continue;  // already deleted
                if (delete_child) {
                    if (it == cc.changed.end())
                        cc.changed.emplace(crid, Change{ReadRow(child, crid), std::nullopt, {}, {}});
                    else
                        it->second.new_row.reset();
                } else if (it == cc.changed.end()) {
                    Row row = ReadRow(child, crid);
                    if (SameKey(row[ccol], target)) continue;
                    Row new_row = row;
                    new_row[ccol] = target;
                    cc.changed.emplace(crid, Change{std::move(row), std::move(new_row), {}, {}});
                } else {
                    Value& v = (*it->second.new_row)[ccol];
                    if (SameKey(v, target)) continue;
                    v = target;
                }
                work.emplace_back(&cc, crid);
            }
        }
    }
}

void WriteSet::Validate() {
    for (auto& [name, tc] : tables_) {
        const Schema& schema = tc.table->schema;
        for (auto& [rid, ch] : tc.changed) {
            if (!ch.new_row) continue;
            // Again: a foreign key action may have changed the row since Update().
            *ch.new_row = PrepareRow(tc.table, std::move(*ch.new_row));
            ch.tuple = SerializeChecked(schema, *ch.new_row);
        }
        for (Insertion& in : tc.inserted) in.tuple = SerializeChecked(schema, in.row);
    }
    for (auto& [name, tc] : tables_) CheckUnique(tc);
    CheckForeignKeys();
}

// A new key conflicts if another new row of the statement has it too, or if a
// row the statement does not touch already has it.
void WriteSet::CheckUnique(TableChanges& tc) {
    auto touched = [&tc](RID r) { return tc.changed.count(r) > 0; };  // their old keys don't count
    for (IndexInfo* idx : catalog_->IndexesOf(tc.table->name)) {
        if (!idx->unique) continue;
        std::unordered_set<std::string> seen;  // NULLs are distinct: a key with one is never a duplicate
        for (auto& [rid, ch] : tc.changed) {
            if (!ch.new_row) continue;
            const Row k = idx->KeyValues(*ch.new_row);
            if (HasNull(k)) continue;
            if (!seen.insert(Encode(k)).second) ThrowDuplicate(idx, k);
            if (SameKey(idx->KeyValues(ch.old_row), k)) continue;  // keeps its own key
            if (catalog_->KeyTaken(idx, k, touched)) ThrowDuplicate(idx, k);
        }
        for (const Insertion& in : tc.inserted) {
            const Row k = idx->KeyValues(in.row);
            if (HasNull(k)) continue;
            if (!seen.insert(Encode(k)).second || catalog_->KeyTaken(idx, k, touched)) ThrowDuplicate(idx, k);
        }
    }
}

const std::unordered_set<std::string>& WriteSet::NewKeys(TableChanges& tc, size_t col) {
    auto [it, fresh] = tc.new_keys.try_emplace(col);
    if (fresh) {
        for (auto& [rid, ch] : tc.changed)
            if (ch.new_row && !(*ch.new_row)[col].IsNull()) it->second.insert(Encode((*ch.new_row)[col]));
        for (const Insertion& in : tc.inserted)
            if (!in.row[col].IsNull()) it->second.insert(Encode(in.row[col]));
    }
    return it->second;
}

bool WriteSet::ParentHasKey(const ForeignKey& fk, const Value& key) {
    TableInfo* parent = catalog_->RequireTable(fk.ref_table);
    IndexInfo* idx = catalog_->UniqueIndexOn(fk.ref_table, fk.ref_column);
    if (!idx) throw DbError("internal: foreign key \"" + fk.name + "\" has no unique index");
    TableChanges* pt = Find(fk.ref_table);
    if (!pt) return catalog_->KeyTaken(idx, key);
    if (NewKeys(*pt, ColumnOf(parent, fk.ref_column)).count(Encode(key))) return true;
    return catalog_->KeyTaken(idx, key, [pt](RID r) { return pt->changed.count(r) > 0; });
}

void WriteSet::CheckForeignKeys() {
    const std::vector<ForeignKey*> fks = catalog_->ForeignKeys();
    // Referencing side: every new or changed non-NULL key must exist in the parent.
    for (ForeignKey* fk : fks) {
        TableChanges* ct = Find(fk->table);
        if (!ct) continue;
        const size_t col = ColumnOf(ct->table, fk->column);
        auto check = [&](const Value& v) {
            if (!v.IsNull() && !ParentHasKey(*fk, v))
                throw DbError("insert or update on table \"" + fk->table + "\" violates foreign key constraint \"" +
                              fk->name + "\": key (" + fk->column + ")=(" + v.ToString() +
                              ") is not present in table \"" + fk->ref_table + "\"");
        };
        for (auto& [rid, ch] : ct->changed)
            if (ch.new_row && !SameKey(ch.old_row[col], (*ch.new_row)[col])) check((*ch.new_row)[col]);
        for (const Insertion& in : ct->inserted) check(in.row[col]);
    }
    // Referenced side: a key that disappears must no longer be referenced.
    for (ForeignKey* fk : fks) {
        TableChanges* pt = Find(fk->ref_table);
        if (!pt) continue;
        TableChanges* ct = Find(fk->table);
        const size_t pcol = ColumnOf(pt->table, fk->ref_column);
        const size_t ccol = ColumnOf(catalog_->RequireTable(fk->table), fk->column);
        for (auto& [rid, ch] : pt->changed) {
            const Value& k = ch.old_row[pcol];
            if (k.IsNull()) continue;
            if (ch.new_row && SameKey(k, (*ch.new_row)[pcol])) continue;
            if (ParentHasKey(*fk, k)) continue;  // another row has the key now
            bool referenced = ct && NewKeys(*ct, ccol).count(Encode(k)) > 0;
            if (!referenced)
                for (RID crid : ReferencingRows(*fk, k))
                    if (!ct || !ct->changed.count(crid)) {
                        referenced = true;
                        break;
                    }
            if (referenced)
                throw DbError("update or delete on table \"" + fk->ref_table + "\" violates foreign key constraint \"" +
                              fk->name + "\" on table \"" + fk->table + "\": key (" + fk->ref_column + ")=(" +
                              k.ToString() + ") is still referenced from table \"" + fk->table + "\"");
        }
    }
}

// Heap first (a tuple that grows may move to a new RID), then per index all
// removals before any insertion, so entries of different rows never clash.
void WriteSet::Write(TableChanges& tc) {
    TableInfo* t = tc.table;
    for (auto& [rid, ch] : tc.changed)
        if (!ch.new_row) {
            t->heap->Delete(rid);
            --t->row_count;
        }
    for (auto& [rid, ch] : tc.changed)
        if (ch.new_row) ch.new_rid = t->heap->Update(rid, ch.tuple);
    std::vector<RID> inserted;
    inserted.reserve(tc.inserted.size());
    for (const Insertion& in : tc.inserted) {
        inserted.push_back(t->heap->Insert(in.tuple));
        ++t->row_count;
    }
    for (IndexInfo* idx : catalog_->IndexesOf(t->name)) {
        auto entry_changes = [idx](RID rid, const Change& ch) {
            return !ch.new_row || ch.new_rid != rid || !SameKey(idx->KeyValues(ch.old_row), idx->KeyValues(*ch.new_row));
        };
        for (auto& [rid, ch] : tc.changed)
            if (entry_changes(rid, ch) && idx->Indexes(ch.old_row))
                idx->tree->Remove(idx->EntryFor(idx->KeyValues(ch.old_row), rid));
        for (auto& [rid, ch] : tc.changed)
            if (ch.new_row && entry_changes(rid, ch) && idx->Indexes(*ch.new_row))
                idx->tree->Insert(idx->EntryFor(idx->KeyValues(*ch.new_row), ch.new_rid));
        for (size_t i = 0; i < tc.inserted.size(); ++i)
            if (idx->Indexes(tc.inserted[i].row))
                idx->tree->Insert(idx->EntryFor(idx->KeyValues(tc.inserted[i].row), inserted[i]));
    }
}

}  // namespace minidb
