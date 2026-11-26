#include "database.h"

#include <algorithm>
#include <chrono>
#include <random>

#include "write_set.h"

namespace minidb {

Database::Database(const std::string& path, size_t pool_frames) {
    disk_ = std::make_unique<DiskManager>(path);
    bpm_ = std::make_unique<BufferPoolManager>(pool_frames, disk_.get());
    catalog_ = std::make_unique<Catalog>(bpm_.get());
    planner_ = std::make_unique<Planner>(catalog_.get());
}

Database::~Database() {
    try {
        Checkpoint();
    } catch (...) {
    }
}

void Database::Checkpoint() {
    catalog_->Save();
    bpm_->FlushAll();
}

std::vector<QueryResult> Database::Execute(const std::string& sql) {
    Parser parser(sql);
    std::vector<Statement> stmts = parser.ParseAll();
    std::vector<QueryResult> results;
    for (const auto& s : stmts) results.push_back(Run(s));
    return results;
}

QueryResult Database::Run(const Statement& stmt) {
    // Read-only statements need no checkpoint.
    if (auto* s = std::get_if<SelectStmt>(&stmt)) return RunSelect(*s);
    if (auto* s = std::get_if<ExplainStmt>(&stmt)) return RunExplain(*s);

    QueryResult r;
    try {
        if (auto* s = std::get_if<CreateTableStmt>(&stmt))
            r = RunCreateTable(*s);
        else if (auto* s2 = std::get_if<CreateIndexStmt>(&stmt))
            r = RunCreateIndex(*s2);
        else if (auto* s3 = std::get_if<DropStmt>(&stmt))
            r = RunDrop(*s3);
        else if (auto* s4 = std::get_if<InsertStmt>(&stmt))
            r = RunInsert(*s4);
        else if (auto* s5 = std::get_if<DeleteStmt>(&stmt))
            r = RunDelete(*s5);
        else if (auto* s6 = std::get_if<UpdateStmt>(&stmt))
            r = RunUpdate(*s6);
        else if (auto* s7 = std::get_if<AlterTableStmt>(&stmt))
            r = RunAlterTable(*s7);
    } catch (...) {
        Checkpoint();  // keep catalog statistics consistent with whatever was written
        throw;
    }
    // No WAL: we simply force everything to disk at the end of each modifying statement.
    Checkpoint();
    return r;
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
    std::string pk;
    for (const auto& d : s.cols) {
        Column c{};
        c.name = d.name;
        c.type = d.type;
        c.not_null = d.not_null;
        c.default_value = EvalDefault(d);
        cols.push_back(c);
        if (d.primary_key) {
            if (!pk.empty()) throw DbError("multiple primary keys for table \"" + s.name + "\" are not allowed");
            pk = d.name;
        }
        if ((d.primary_key || d.unique) && d.type != TypeId::INT && d.type != TypeId::TEXT)
            throw DbError("PRIMARY KEY / UNIQUE is only supported on INT and TEXT columns");
    }
    catalog_->CreateTable(s.name, cols);
    // Indexes and foreign keys need the table to exist (a foreign key may refer
    // to the table itself), so a failure here takes the new table back out.
    try {
        for (const auto& d : s.cols) {
            if (d.primary_key)
                catalog_->CreateIndex(s.name + "_pkey", s.name, d.name, true);
            else if (d.unique)
                catalog_->CreateIndex(s.name + "_" + d.name + "_key", s.name, d.name, true);
        }
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
    catalog_->CreateIndex(s.name, s.table, s.column, s.unique);
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
            if ((d.primary_key || d.unique) && d.type != TypeId::INT && d.type != TypeId::TEXT)
                throw DbError("PRIMARY KEY / UNIQUE is only supported on INT and TEXT columns");
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

QueryResult Database::RunDrop(const DropStmt& s) {
    if (s.index) {
        catalog_->DropIndex(s.name);
        return {{}, {}, "DROP INDEX"};
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
            for (const auto& e : exprs) r.push_back(Evaluate(*BindExpr(planner_->ResolveSubqueries(e), empty), {}));
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
        sets.emplace_back(static_cast<size_t>(idx), BindExpr(planner_->ResolveSubqueries(e), schema));
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
    TableInfo* t = catalog_->RequireTable(table);
    const Schema& schema = t->schema;
    std::mt19937 rng(seed + static_cast<uint32_t>(t->row_count));
    static const char* words[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
                                  "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa"};

    // Columns with a unique index (and the first INT column) get sequential ids;
    // unique TEXT columns get "<column>_<n>", skipping values that are taken.
    std::vector<bool> sequential(schema.Size(), false);
    std::vector<IndexInfo*> unique_idx(schema.Size(), nullptr);
    for (IndexInfo* idx : catalog_->IndexesOf(table))
        if (idx->unique) {
            sequential[static_cast<size_t>(idx->col_idx)] = true;
            unique_idx[static_cast<size_t>(idx->col_idx)] = idx;
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
                        } while (catalog_->KeyTaken(unique_idx[i], v));
                        r.push_back(std::move(v));
                    } else {
                        r.push_back(Value::Text(std::string(words[rng() % 16]) + "_" + std::to_string(rng() % 1000)));
                    }
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
    Checkpoint();
    return {{}, {}, "INSERT " + std::to_string(n)};
}

}  // namespace minidb
