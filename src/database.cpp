#include "database.h"

#include <algorithm>
#include <chrono>
#include <random>

#include "write_set.h"

namespace minidb {

Database::Database(const std::string& path, size_t pool_frames, DiskOptions disk_options) {
    disk_ = std::make_unique<DiskManager>(path, disk_options);  // recovers the write-ahead log
    bpm_ = std::make_unique<BufferPoolManager>(pool_frames, disk_.get());
    catalog_ = std::make_unique<Catalog>(bpm_.get());
    planner_ = std::make_unique<Planner>(catalog_.get());
    Commit();  // a newly created database is durable before its first statement
}

Database::~Database() {
    try {
        if (in_txn_ && broken_.empty()) {
            Undo([this] { disk_->Rollback(); });  // never commit implicitly
            EndTransaction();
        }
        if (broken_.empty()) {
            Checkpoint();  // clean shutdown
            return;
        }
    } catch (...) {
    }
    // Something failed: keep only what was committed. The buffer pool may still
    // write dirty pages into the log when it goes, so it goes first, and then
    // the log forgets everything uncommitted before the DiskManager shuts down.
    try {
        planner_.reset();
        catalog_.reset();
        bpm_.reset();
        disk_->Rollback();
    } catch (...) {
    }
}

// ---- transactions ----

void Database::FlushToLog() {
    catalog_->Save();
    bpm_->FlushAll();  // into the log, not the database file
}

void Database::Commit() {
    FlushToLog();
    disk_->Commit();  // COMMIT record + fsync: from here on the changes survive a crash
}

void Database::Undo(const std::function<void()>& rollback_log) {
    try {
        bpm_->DiscardAll();  // any cached page, dirty or not, may hold undone changes
        rollback_log();
        catalog_->Reload();
    } catch (const std::exception& e) {
        broken_ = e.what();
    }
}

void Database::CheckUsable() const {
    if (!broken_.empty())
        throw DbError("the database cannot be used after a failed rollback (" + broken_ +
                      "); reopen it to recover from the write-ahead log");
}

void Database::Checkpoint() {
    CheckUsable();
    // A checkpoint copies committed pages only; inside a transaction it would
    // have to commit first.
    if (in_txn_) throw DbError("cannot checkpoint inside a transaction (COMMIT or ROLLBACK first)");
    Commit();
    disk_->Checkpoint();
}

QueryResult Database::Modify(const std::function<QueryResult()>& body) {
    QueryResult r;
    if (!in_txn_) {
        // Autocommit: the statement is a transaction of its own.
        try {
            r = body();
            Commit();
        } catch (...) {
            // Checks run before writes, so most failing statements wrote nothing:
            // then there is nothing to undo (and the catalog objects stay valid).
            if (bpm_->HasDirtyPages() || disk_->HasUncommittedChanges()) Undo([this] { disk_->Rollback(); });
            throw;
        }
        return r;
    }
    // Inside a transaction: the statement runs under a savepoint of its own, so
    // a failure undoes just this statement and the transaction goes on. The
    // savepoint must see every change made so far, so they all go to the log.
    FlushToLog();
    disk_->Savepoint();
    try {
        r = body();
    } catch (...) {
        Undo([this] {
            disk_->RollbackToSavepoint();
            disk_->ReleaseSavepoint();
        });
        throw;
    }
    disk_->ReleaseSavepoint();
    return r;
}

void Database::EndTransaction() {
    in_txn_ = false;
    savepoints_.clear();
}

size_t Database::FindSavepoint(const std::string& name) const {
    // The most recent savepoint of that name wins (names may be reused).
    for (size_t i = savepoints_.size(); i-- > 0;)
        if (savepoints_[i] == name) return i;
    throw DbError("savepoint \"" + name + "\" does not exist");
}

QueryResult Database::RunTransaction(const TransactionStmt& s) {
    using K = TransactionStmt::Kind;
    auto require_txn = [this](const char* what) {
        if (!in_txn_) throw DbError(std::string(what) + ": there is no transaction in progress");
    };
    switch (s.kind) {
        case K::BEGIN:
            if (in_txn_) throw DbError("there is already a transaction in progress");
            // Every statement before this one is committed, so the log's current
            // group is empty: from here on it collects this transaction.
            in_txn_ = true;
            return {{}, {}, "BEGIN"};
        case K::COMMIT:
            require_txn("COMMIT");
            try {
                Commit();  // one COMMIT record for the whole transaction
            } catch (const std::exception& e) {
                Undo([this] { disk_->Rollback(); });
                EndTransaction();
                CheckUsable();  // if even the rollback failed, say so instead
                throw DbError(std::string("COMMIT failed, the transaction was rolled back: ") + e.what());
            }
            EndTransaction();
            return {{}, {}, "COMMIT"};
        case K::ROLLBACK:
            require_txn("ROLLBACK");
            Undo([this] { disk_->Rollback(); });  // the log forgets the whole group
            EndTransaction();
            CheckUsable();
            return {{}, {}, "ROLLBACK"};
        case K::SAVEPOINT:
            require_txn("SAVEPOINT");
            FlushToLog();  // the savepoint must see every change made so far
            disk_->Savepoint();
            savepoints_.push_back(s.savepoint);
            return {{}, {}, "SAVEPOINT"};
        case K::RELEASE: {
            require_txn("RELEASE SAVEPOINT");
            // Releases the savepoint and every one created after it.
            const size_t i = FindSavepoint(s.savepoint);
            while (savepoints_.size() > i) {
                disk_->ReleaseSavepoint();
                savepoints_.pop_back();
            }
            return {{}, {}, "RELEASE"};
        }
        case K::ROLLBACK_TO: {
            require_txn("ROLLBACK TO SAVEPOINT");
            // Savepoints created after this one are gone; this one stays.
            const size_t i = FindSavepoint(s.savepoint);
            while (savepoints_.size() > i + 1) {
                disk_->ReleaseSavepoint();
                savepoints_.pop_back();
            }
            Undo([this] { disk_->RollbackToSavepoint(); });
            CheckUsable();
            return {{}, {}, "ROLLBACK"};
        }
    }
    throw DbError("internal: unknown transaction statement");
}

// ---- statements ----

std::vector<QueryResult> Database::Execute(const std::string& sql) {
    Parser parser(sql);
    std::vector<Statement> stmts = parser.ParseAll();
    std::vector<QueryResult> results;
    for (const auto& s : stmts) results.push_back(Run(s));
    return results;
}

QueryResult Database::Run(const Statement& stmt) {
    CheckUsable();
    if (auto* s = std::get_if<TransactionStmt>(&stmt)) return RunTransaction(*s);
    // Read-only statements need neither a commit nor a savepoint.
    if (auto* s = std::get_if<SelectStmt>(&stmt)) return RunSelect(*s);
    if (auto* s = std::get_if<ExplainStmt>(&stmt)) return RunExplain(*s);

    return Modify([&]() -> QueryResult {
        if (auto* s = std::get_if<CreateTableStmt>(&stmt)) return RunCreateTable(*s);
        if (auto* s = std::get_if<CreateIndexStmt>(&stmt)) return RunCreateIndex(*s);
        if (auto* s = std::get_if<DropStmt>(&stmt)) return RunDrop(*s);
        if (auto* s = std::get_if<InsertStmt>(&stmt)) return RunInsert(*s);
        if (auto* s = std::get_if<DeleteStmt>(&stmt)) return RunDelete(*s);
        if (auto* s = std::get_if<UpdateStmt>(&stmt)) return RunUpdate(*s);
        if (auto* s = std::get_if<AlterTableStmt>(&stmt)) return RunAlterTable(*s);
        if (auto* s = std::get_if<CreateViewStmt>(&stmt)) return RunCreateView(*s);
        if (auto* s = std::get_if<AnalyzeStmt>(&stmt)) {
            if (!s->table.empty()) {
                catalog_->Analyze(catalog_->RequireTable(s->table));
            } else {
                for (TableInfo* t : catalog_->Tables()) catalog_->Analyze(t);
            }
            return QueryResult{{}, {}, "ANALYZE"};
        }
        throw DbError("internal: unknown statement");
    });
}

// Evaluates a column's DEFAULT once, when the column is defined: it must be a
// constant expression, it is stored cast to the column type, and a NOT NULL
// column cannot default to NULL.
std::optional<Value> Database::EvalDefault(const ColumnDef& d) {
    if (!d.default_expr) return std::nullopt;
    if (ContainsAggregate(*d.default_expr)) throw DbError("DEFAULT cannot contain aggregate functions");
    Value v;
    try {
        v = Evaluate(*BindExpr(planner_->ResolveSubqueries(d.default_expr), Schema{}), {});
    } catch (const DbError& e) {
        throw DbError("invalid DEFAULT for column \"" + d.name + "\" (it must be a constant): " + e.what());
    }
    v = v.CastTo(d.type);
    if (v.IsNull() && d.not_null) throw DbError("column \"" + d.name + "\" is NOT NULL but its DEFAULT is NULL");
    return v;
}

QueryResult Database::RunCreateTable(const CreateTableStmt& s) {
    std::vector<Column> cols;
    // The primary key (from a column or the table constraint) and the UNIQUE
    // constraints, each a list of columns. Each becomes a unique index.
    std::vector<std::string> pk = s.primary_key;
    std::vector<std::vector<std::string>> unique_keys;
    for (const auto& d : s.cols) {
        Column c{};
        c.name = d.name;
        c.type = d.type;
        c.not_null = d.not_null;
        c.default_value = EvalDefault(d);
        cols.push_back(c);
        if (d.primary_key) {
            if (!pk.empty()) throw DbError("multiple primary keys for table \"" + s.name + "\" are not allowed");
            pk = {d.name};
        }
        if (d.unique) unique_keys.push_back({d.name});
    }
    unique_keys.insert(unique_keys.end(), s.unique_keys.begin(), s.unique_keys.end());

    auto check_types = [&](const std::vector<std::string>& key) {
        for (const auto& name : key)
            for (const auto& d : s.cols)
                if (d.name == name && !IsIndexable(d.type))
                    throw DbError("PRIMARY KEY / UNIQUE is only supported on INT, TEXT, DATE and TIMESTAMP columns");
    };
    check_types(pk);
    for (const auto& key : unique_keys) check_types(key);

    // Index names as in PostgreSQL: t_pkey, t_a_key, t_a_b_key. A UNIQUE constraint
    // on exactly the same columns as the primary key or an earlier one adds nothing.
    std::vector<std::pair<std::string, std::vector<std::string>>> indexes;
    if (!pk.empty()) indexes.emplace_back(s.name + "_pkey", pk);
    for (const auto& key : unique_keys) {
        bool redundant = false;
        for (const auto& [n, cols_of] : indexes) redundant = redundant || cols_of == key;
        if (redundant) continue;
        std::string name = s.name;
        for (const auto& c : key) name += "_" + c;
        indexes.emplace_back(name + "_key", key);
    }

    catalog_->CreateTable(s.name, cols);
    // Indexes and foreign keys need the table to exist (a foreign key may refer
    // to the table itself), so a failure here takes the new table back out.
    try {
        for (const auto& [name, key] : indexes) catalog_->CreateIndex(name, s.name, key, true);
        for (const auto& d : s.cols) {
            if (!d.references) continue;
            ForeignKey fk = *d.references;
            fk.column = d.name;
            AddForeignKey(s.name, fk);
        }
        for (const auto& fk : s.foreign_keys) AddForeignKey(s.name, fk);
    } catch (...) {
        try {
            catalog_->DropTable(s.name);
        } catch (...) {
        }
        throw;
    }
    return {{}, {}, "CREATE TABLE"};
}

void Database::AddForeignKey(const std::string& table, ForeignKey fk) {
    fk.table = table;
    if (fk.name.empty()) fk.name = catalog_->NewForeignKeyName(table, fk.column);
    catalog_->CreateForeignKey(std::move(fk));
}

QueryResult Database::RunCreateIndex(const CreateIndexStmt& s) {
    catalog_->CreateIndex(s.name, s.table, s.columns, s.unique);
    return {{}, {}, "CREATE INDEX"};
}

QueryResult Database::RunAlterTable(const AlterTableStmt& s) {
    TableInfo* t = catalog_->RequireTable(s.table);
    using A = AlterTableStmt::Action;
    switch (s.action) {
        case A::RENAME_TABLE: catalog_->RenameTable(s.table, s.new_name); break;
        case A::RENAME_COLUMN: catalog_->RenameColumn(s.table, s.name, s.new_name); break;
        case A::ADD_COLUMN: {
            const ColumnDef& d = s.column;
            if (t->schema.Find("", d.name) >= 0)
                throw DbError("column \"" + d.name + "\" of relation \"" + s.table + "\" already exists");
            if ((d.primary_key || d.unique) && !IsIndexable(d.type))
                throw DbError("PRIMARY KEY / UNIQUE is only supported on INT, TEXT, DATE and TIMESTAMP columns");
            // Existing rows get the DEFAULT (or NULL), so NOT NULL needs a non-NULL
            // DEFAULT unless the table is empty, and a UNIQUE column cannot give the
            // same DEFAULT to several rows.
            std::optional<Value> def = EvalDefault(d);
            bool null_fill = !def || def->IsNull();
            if (d.not_null && null_fill && t->row_count > 0)
                throw DbError("column \"" + d.name + "\" cannot be NOT NULL: \"" + s.table +
                              "\" already has rows, which would get NULL (add a DEFAULT)");
            if ((d.unique || d.primary_key) && !null_fill && t->row_count > 1)
                throw DbError("column \"" + d.name + "\" cannot be UNIQUE: its DEFAULT would be the same in every existing row");
            std::string pk_index = s.table + "_pkey";
            if (d.primary_key) {
                for (IndexInfo* i : catalog_->IndexesOf(s.table))
                    if (i->name == pk_index)
                        throw DbError("multiple primary keys for table \"" + s.table + "\" are not allowed");
            }
            // A REFERENCES clause is fully checked before the table is rewritten:
            // the target, and that the value every existing row gets is present.
            std::optional<ForeignKey> fk;
            if (d.references) {
                fk = *d.references;
                fk->table = s.table;
                fk->column = d.name;
                if (fk->name.empty()) fk->name = catalog_->NewForeignKeyName(s.table, d.name);
                if (catalog_->GetForeignKey(fk->name)) throw DbError("constraint \"" + fk->name + "\" already exists");
                catalog_->ResolveForeignKey(&*fk, d.type);
                if (!null_fill && t->row_count > 0) {
                    Value key = def->CastTo(d.type);
                    if (!catalog_->KeyTaken(catalog_->UniqueIndexOn(fk->ref_table, fk->ref_column), key))
                        throw DbError("column \"" + d.name + "\" cannot reference \"" + fk->ref_table +
                                      "\": its DEFAULT " + key.ToString() + " is not present there");
                }
                if ((fk->on_delete == FkAction::SET_NULL || fk->on_update == FkAction::SET_NULL) && d.not_null)
                    throw DbError("foreign key constraint \"" + fk->name + "\" cannot SET NULL: column \"" + d.name +
                                  "\" is NOT NULL");
            }
            std::vector<Column> cols = t->schema.Columns();
            Column c{};
            c.name = d.name;
            c.type = d.type;
            c.not_null = d.not_null;
            c.default_value = def;
            cols.push_back(c);
            Value fill = def ? *def : Value::Null();
            catalog_->RewriteTable(t, cols, [&fill](Row r) {
                r.push_back(fill);
                return r;
            });
            if (d.primary_key)
                catalog_->CreateIndex(pk_index, s.table, d.name, true);
            else if (d.unique)
                catalog_->CreateIndex(s.table + "_" + d.name + "_key", s.table, d.name, true);
            if (fk) catalog_->CreateForeignKey(*fk);
            break;
        }
        case A::DROP_COLUMN: {
            int col = t->schema.Find("", s.name);
            if (col < 0) throw DbError("column \"" + s.name + "\" of relation \"" + s.table + "\" does not exist");
            if (t->schema.Size() == 1) throw DbError("cannot drop the only column of table \"" + s.table + "\"");
            // A referenced column can't go; the column's own foreign keys go with it.
            for (ForeignKey* fk : catalog_->ForeignKeysTo(s.table))
                if (fk->ref_column == s.name && !(fk->table == s.table && fk->column == s.name))
                    throw DbError("cannot drop column \"" + s.name + "\" of table \"" + s.table +
                                  "\" because foreign key constraint \"" + fk->name + "\" on table \"" + fk->table +
                                  "\" references it");
            std::vector<Column> cols = t->schema.Columns();
            cols.erase(cols.begin() + col);
            catalog_->RewriteTable(t, cols, [col](Row r) {
                r.erase(r.begin() + col);
                return r;
            });
            for (ForeignKey* fk : catalog_->ForeignKeysOf(s.table))
                if (fk->column == s.name) catalog_->DropForeignKey(std::string(fk->name));
            break;
        }
        case A::ADD_FOREIGN_KEY: AddForeignKey(s.table, s.foreign_key); break;
        case A::DROP_CONSTRAINT: {
            ForeignKey* fk = catalog_->GetForeignKey(s.name);
            if (!fk || fk->table != s.table)
                throw DbError("constraint \"" + s.name + "\" of relation \"" + s.table + "\" does not exist");
            catalog_->DropForeignKey(s.name);
            break;
        }
    }
    return {{}, {}, "ALTER TABLE"};
}

QueryResult Database::RunCreateView(const CreateViewStmt& s) {
    // Plan (without running) the query once, so a view that could never work is
    // rejected now: unknown tables or columns, a wrong number of column names,
    // duplicate column names, or (with OR REPLACE) a view that would read itself.
    // A name clash is reported first, before any error in the query.
    if (catalog_->GetTable(s.name)) throw DbError("a table named \"" + s.name + "\" already exists");
    if (!s.or_replace && catalog_->GetView(s.name)) throw DbError("view \"" + s.name + "\" already exists");
    Schema out;
    planner_->PlanView(s.name, s.columns, *s.select, &out);
    catalog_->CreateView(s.name, s.columns, s.sql, s.or_replace);
    return {{}, {}, "CREATE VIEW"};
}

QueryResult Database::RunDrop(const DropStmt& s) {
    if (s.index) {
        catalog_->DropIndex(s.name);
        return {{}, {}, "DROP INDEX"};
    }
    if (s.view) {
        catalog_->DropView(s.name);
        return {{}, {}, "DROP VIEW"};
    }
    catalog_->DropTable(s.name);
    return {{}, {}, "DROP TABLE"};
}

void Database::InsertRows(TableInfo* t, const std::vector<Row>& rows) {
    WriteSet ws(catalog_.get());
    for (const Row& r : rows) ws.Insert(t, r);
    ws.Apply();
}

QueryResult Database::RunInsert(const InsertStmt& s) {
    TableInfo* t = catalog_->RequireTable(s.table);
    const Schema& schema = t->schema;

    std::vector<size_t> positions;
    if (s.columns.empty()) {
        for (size_t i = 0; i < schema.Size(); ++i) positions.push_back(i);
    } else {
        for (const auto& c : s.columns) {
            int idx = schema.Find("", c);
            if (idx < 0) throw DbError("column \"" + c + "\" of relation \"" + s.table + "\" does not exist");
            if (std::find(positions.begin(), positions.end(), static_cast<size_t>(idx)) != positions.end())
                throw DbError("column \"" + c + "\" specified more than once");
            positions.push_back(static_cast<size_t>(idx));
        }
    }

    std::vector<Row> source;
    if (s.select) {
        ExecPtr plan = planner_->PlanSelect(*s.select);
        plan->Init();
        Row r;
        while (plan->Next(&r)) source.push_back(r);
    } else {
        Schema empty;
        for (const auto& exprs : s.rows) {
            Row r;
            for (const auto& e : exprs) r.push_back(Evaluate(*BindExpr(planner_->ResolveSubqueries(e, t), empty), {}));
            source.push_back(std::move(r));
        }
    }

    std::vector<Row> rows;
    rows.reserve(source.size());
    for (const auto& src : source) {
        if (src.size() != positions.size())
            throw DbError("INSERT has " + std::to_string(src.size()) + " values but " +
                          std::to_string(positions.size()) + " target columns");
        Row full(schema.Size());
        for (size_t i = 0; i < schema.Size(); ++i)  // columns left out get their DEFAULT (or NULL)
            if (schema.At(i).default_value) full[i] = *schema.At(i).default_value;
        for (size_t i = 0; i < positions.size(); ++i) full[positions[i]] = src[i];
        rows.push_back(std::move(full));
    }
    InsertRows(t, rows);
    return {{}, {}, "INSERT " + std::to_string(rows.size())};
}

QueryResult Database::RunSelect(const SelectStmt& s) {
    ExecPtr plan = planner_->PlanSelect(s);
    QueryResult r;
    for (const auto& c : plan->OutSchema().Columns()) r.columns.push_back(c.name);
    plan->Init();
    Row row;
    while (plan->Next(&row)) r.rows.push_back(row);
    r.message = "(" + std::to_string(r.rows.size()) + (r.rows.size() == 1 ? " row)" : " rows)");
    return r;
}

QueryResult Database::RunExplain(const ExplainStmt& s) {
    ExecPtr plan = planner_->PlanSelect(*s.select);
    double ms = 0;
    if (s.analyze) {
        auto t0 = std::chrono::steady_clock::now();
        plan->Init();
        Row row;
        while (plan->Next(&row)) {
        }
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    QueryResult r;
    r.columns = {"QUERY PLAN"};
    for (const auto& line : Planner::Explain(*plan, s.analyze)) r.rows.push_back({Value::Text(line)});
    if (s.analyze) r.rows.push_back({Value::Text("Execution Time: " + std::to_string(ms) + " ms")});
    r.message = "";
    return r;
}

QueryResult Database::RunDelete(const DeleteStmt& s) {
    TableInfo* t = catalog_->RequireTable(s.table);
    auto scan = planner_->PlanTableScan(t, s.where);
    // Collect the victims first, so we never modify the structure we are scanning.
    WriteSet ws(catalog_.get());
    size_t n = 0;
    scan->Init();
    Row row;
    while (scan->Next(&row)) {
        ws.Delete(t, scan->CurrentRid(), row);
        ++n;
    }
    ws.Apply();  // ON DELETE actions may change or delete rows of other tables too
    return {{}, {}, "DELETE " + std::to_string(n)};
}

QueryResult Database::RunUpdate(const UpdateStmt& s) {
    TableInfo* t = catalog_->RequireTable(s.table);
    const Schema& schema = t->schema;
    std::vector<std::pair<size_t, ExprPtr>> sets;
    for (const auto& [col, e] : s.sets) {
        int idx = schema.Find("", col);
        if (idx < 0) throw DbError("column \"" + col + "\" of relation \"" + s.table + "\" does not exist");
        for (const auto& prev : sets)
            if (prev.first == static_cast<size_t>(idx))
                throw DbError("multiple assignments to same column \"" + col + "\"");
        if (ContainsAggregate(*e)) throw DbError("aggregate functions are not allowed in UPDATE");
        sets.emplace_back(static_cast<size_t>(idx), planner_->PlanRowExpr(t, e));  // may be correlated
    }

    auto scan = planner_->PlanTableScan(t, s.where);
    std::vector<std::pair<RID, Row>> targets;
    scan->Init();
    Row row;
    while (scan->Next(&row)) targets.emplace_back(scan->CurrentRid(), row);

    // Compute every new row; nothing is written before ws.Apply() has checked
    // them all, so any error (division by zero, NOT NULL, ...) changes nothing.
    WriteSet ws(catalog_.get());
    for (auto& [rid, old_row] : targets) {
        Row new_row = old_row;
        for (auto& [idx, e] : sets) new_row[idx] = Evaluate(*e, old_row);
        ws.Update(t, rid, old_row, std::move(new_row));
    }
    ws.Apply();
    return {{}, {}, "UPDATE " + std::to_string(targets.size())};
}

QueryResult Database::Populate(const std::string& table, int64_t n, uint32_t seed) {
    CheckUsable();
    return Modify([&]() -> QueryResult {
        PopulateRows(table, n, seed);
        return {{}, {}, "INSERT " + std::to_string(n)};
    });
}

void Database::PopulateRows(const std::string& table, int64_t n, uint32_t seed) {
    TableInfo* t = catalog_->RequireTable(table);
    const Schema& schema = t->schema;
    std::mt19937 rng(seed + static_cast<uint32_t>(t->row_count));
    static const char* words[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
                                  "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa"};

    // Columns with a unique index (and the first INT column) get sequential ids;
    // unique TEXT columns get "<column>_<n>", skipping values that are taken. A
    // multi-column unique index needs just one sequential column (preferably an
    // INT one: it starts above every existing value, so no key can be taken).
    std::vector<bool> sequential(schema.Size(), false);
    std::vector<IndexInfo*> unique_idx(schema.Size(), nullptr);
    for (IndexInfo* idx : catalog_->IndexesOf(table)) {
        if (!idx->unique) continue;
        auto col = static_cast<size_t>(idx->col_idxs[0]);
        if (idx->Composite()) {
            for (int c : idx->col_idxs)
                if (schema.At(static_cast<size_t>(c)).type == TypeId::INT) {
                    col = static_cast<size_t>(c);
                    break;
                }
        } else {
            unique_idx[col] = idx;
        }
        sequential[col] = true;
    }
    if (!schema.Columns().empty() && schema.At(0).type == TypeId::INT) sequential[0] = true;
    // Foreign key columns (unless unique) get keys picked from the referenced
    // table as it is now, or NULL if it is empty.
    std::vector<std::optional<std::vector<Value>>> fk_keys(schema.Size());
    for (ForeignKey* fk : catalog_->ForeignKeysOf(table)) {
        const auto col = static_cast<size_t>(schema.Find("", fk->column));
        if (unique_idx[col] || fk_keys[col]) continue;
        TableInfo* parent = catalog_->RequireTable(fk->ref_table);
        const auto pcol = static_cast<size_t>(parent->schema.Find("", fk->ref_column));
        auto& keys = fk_keys[col].emplace();
        auto it = parent->heap->Begin();
        RID rid;
        std::string buf;
        constexpr size_t kMaxKeys = size_t{1} << 20;
        while (keys.size() < kMaxKeys && it.Next(&rid, &buf)) {
            Row r = DeserializeTuple(parent->schema, buf.data(), buf.size());
            if (!r[pcol].IsNull()) keys.push_back(std::move(r[pcol]));
        }
        sequential[col] = false;
    }
    std::vector<int64_t> next_id(schema.Size(), 1);
    {
        auto it = t->heap->Begin();
        RID rid;
        std::string buf;
        while (it.Next(&rid, &buf)) {
            Row r = DeserializeTuple(schema, buf.data(), buf.size());
            for (size_t i = 0; i < schema.Size(); ++i)
                if (sequential[i] && schema.At(i).type == TypeId::INT && !r[i].IsNull())
                    next_id[i] = std::max(next_id[i], r[i].AsInt() + 1);
        }
    }

    // Rows are inserted in batches to bound memory use for very large n.
    constexpr int64_t kBatch = 4096;
    std::vector<Row> batch;
    for (int64_t k = 0; k < n; ++k) {
        Row r;
        for (size_t i = 0; i < schema.Size(); ++i) {
            if (fk_keys[i]) {
                const auto& keys = *fk_keys[i];
                r.push_back(keys.empty() ? Value::Null() : keys[rng() % keys.size()]);
                continue;
            }
            switch (schema.At(i).type) {
                case TypeId::INT:
                    r.push_back(Value::Int(sequential[i] ? next_id[i]++ : static_cast<int64_t>(rng() % 1000)));
                    break;
                case TypeId::DOUBLE: r.push_back(Value::Double(static_cast<double>(rng() % 100000) / 100.0)); break;
                case TypeId::BOOL: r.push_back(Value::Bool(rng() % 2 == 0)); break;
                case TypeId::TEXT:
                    if (sequential[i]) {
                        Value v;
                        do {
                            v = Value::Text(schema.At(i).name + "_" + std::to_string(next_id[i]++));
                        } while (unique_idx[i] && catalog_->KeyTaken(unique_idx[i], v));
                        r.push_back(std::move(v));
                    } else {
                        r.push_back(Value::Text(std::string(words[rng() % 16]) + "_" + std::to_string(rng() % 1000)));
                    }
                    break;
                case TypeId::DATE: r.push_back(Value::Date(DaysFromCivil(2020, 1, 1) + static_cast<int64_t>(rng() % 3650))); break;
                case TypeId::TIMESTAMP:
                    r.push_back(Value::Timestamp(DaysFromCivil(2020, 1, 1) * kSecondsPerDay +
                                                 static_cast<int64_t>(rng() % (3650 * kSecondsPerDay))));
                    break;
                case TypeId::NULL_T: r.push_back(Value::Null()); break;
            }
        }
        batch.push_back(std::move(r));
        if (static_cast<int64_t>(batch.size()) == kBatch || k + 1 == n) {
            InsertRows(t, batch);
            batch.clear();
        }
    }
}

}  // namespace minidb
