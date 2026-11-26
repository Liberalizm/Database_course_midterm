#include "database.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <set>
#include <unordered_set>

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
    for (const auto& d : s.cols) {
        if (d.primary_key)
            catalog_->CreateIndex(s.name + "_pkey", s.name, d.name, true);
        else if (d.unique)
            catalog_->CreateIndex(s.name + "_" + d.name + "_key", s.name, d.name, true);
    }
    return {{}, {}, "CREATE TABLE"};
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
            break;
        }
        case A::DROP_COLUMN: {
            int col = t->schema.Find("", s.name);
            if (col < 0) throw DbError("column \"" + s.name + "\" of relation \"" + s.table + "\" does not exist");
            if (t->schema.Size() == 1) throw DbError("cannot drop the only column of table \"" + s.table + "\"");
            std::vector<Column> cols = t->schema.Columns();
            cols.erase(cols.begin() + col);
            catalog_->RewriteTable(t, cols, [col](Row r) {
                r.erase(r.begin() + col);
                return r;
            });
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

namespace {

[[noreturn]] void ThrowDuplicate(const IndexInfo* idx, const Value& key) {
    throw DbError("duplicate key value violates unique constraint \"" + idx->name + "\": " + idx->column + "=" +
                  key.ToString());
}

std::string SerializeChecked(const Schema& schema, const Row& row) {
    std::string tuple = SerializeTuple(schema, row);
    if (tuple.size() > TablePage::MAX_TUPLE)
        throw DbError("row is too large (" + std::to_string(tuple.size()) + " bytes, max " +
                      std::to_string(TablePage::MAX_TUPLE) + ")");
    return tuple;
}

bool SameKey(const Value& a, const Value& b) {
    return a.IsNull() == b.IsNull() && (a.IsNull() || a.Compare(b) == 0);
}

}  // namespace

// Casts a full row to the column types and enforces NOT NULL.
Row Database::PrepareRow(const TableInfo* t, Row row) const {
    const Schema& schema = t->schema;
    for (size_t i = 0; i < schema.Size(); ++i) {
        row[i] = row[i].CastTo(schema.At(i).type);
        if (row[i].IsNull() && schema.At(i).not_null)
            throw DbError("null value in column \"" + schema.At(i).name + "\" violates not-null constraint");
    }
    return row;
}

// Statement atomicity without a log: every check that can fail runs before the
// first byte is written, so a failing statement leaves the table untouched.
void Database::InsertRows(TableInfo* t, const std::vector<Row>& input) {
    const Schema& schema = t->schema;
    std::vector<Row> rows;
    std::vector<std::string> tuples;
    rows.reserve(input.size());
    tuples.reserve(input.size());
    for (const Row& r : input) {
        rows.push_back(PrepareRow(t, r));
        tuples.push_back(SerializeChecked(schema, rows.back()));
    }

    auto indexes = catalog_->IndexesOf(t->name);
    for (IndexInfo* idx : indexes) {
        if (!idx->unique) continue;
        std::unordered_set<std::string> seen;  // duplicates inside this statement (by value)
        for (const Row& r : rows) {
            const Value& v = r[static_cast<size_t>(idx->col_idx)];
            if (v.IsNull()) continue;
            std::string vk;
            v.AppendKey(&vk);
            if (!seen.insert(std::move(vk)).second || catalog_->KeyTaken(idx, v)) ThrowDuplicate(idx, v);
        }
    }

    for (size_t i = 0; i < rows.size(); ++i) {
        RID rid = t->heap->Insert(tuples[i]);
        for (IndexInfo* idx : indexes) {
            const Value& v = rows[i][static_cast<size_t>(idx->col_idx)];
            if (!v.IsNull()) idx->tree->Insert(idx->KeyOf(v), rid);
        }
        ++t->row_count;
    }
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
    // Phase 1: collect victims (so we never modify the structure we are scanning).
    std::vector<std::pair<RID, Row>> victims;
    scan->Init();
    Row row;
    while (scan->Next(&row)) victims.emplace_back(scan->CurrentRid(), row);
    // Phase 2: delete from indexes and heap.
    auto indexes = catalog_->IndexesOf(t->name);
    for (auto& [rid, r] : victims) {
        for (IndexInfo* idx : indexes) {
            const Value& v = r[static_cast<size_t>(idx->col_idx)];
            if (!v.IsNull()) idx->tree->Remove(idx->KeyOf(v), rid);
        }
        t->heap->Delete(rid);
        --t->row_count;
    }
    return {{}, {}, "DELETE " + std::to_string(victims.size())};
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

    // Phase 1: compute and validate every new row. Nothing is written yet, so any
    // error here (division by zero, NOT NULL, row too large, ...) changes nothing.
    std::vector<Row> new_rows;
    std::vector<std::string> tuples;
    new_rows.reserve(targets.size());
    tuples.reserve(targets.size());
    for (auto& [rid, old_row] : targets) {
        Row new_row = old_row;
        for (auto& [idx, e] : sets) new_row[idx] = Evaluate(*e, old_row);
        new_rows.push_back(PrepareRow(t, std::move(new_row)));
        tuples.push_back(SerializeChecked(schema, new_rows.back()));
    }

    // Uniqueness is checked against the state *after* the statement (as the SQL
    // standard requires), so "SET id = id + 1" works although rows collide midway.
    // A new key conflicts if another target gets it too, or if a row that is not
    // being updated already has it.
    auto indexes = catalog_->IndexesOf(t->name);
    std::set<RID> target_rids;
    for (const auto& tr : targets) target_rids.insert(tr.first);
    for (IndexInfo* ix : indexes) {
        if (!ix->unique) continue;
        const auto col = static_cast<size_t>(ix->col_idx);
        std::unordered_set<std::string> seen;
        for (size_t i = 0; i < targets.size(); ++i) {
            const Value& nv = new_rows[i][col];
            if (nv.IsNull()) continue;
            std::string vk;
            nv.AppendKey(&vk);
            if (!seen.insert(std::move(vk)).second) ThrowDuplicate(ix, nv);
            if (SameKey(targets[i].second[col], nv)) continue;  // keeps its own key
            if (catalog_->KeyTaken(ix, nv, [&](RID r) { return target_rids.count(r) > 0; })) ThrowDuplicate(ix, nv);
        }
    }

    // Phase 2: apply. Heap first (a tuple that grows may move to a new RID), then
    // all index removals before any insertion, so entries of different rows can
    // never clash with each other mid-way.
    std::vector<RID> new_rids;
    new_rids.reserve(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) new_rids.push_back(t->heap->Update(targets[i].first, tuples[i]));
    for (IndexInfo* ix : indexes) {
        const auto col = static_cast<size_t>(ix->col_idx);
        auto changed = [&](size_t i) {
            return new_rids[i] != targets[i].first || !SameKey(targets[i].second[col], new_rows[i][col]);
        };
        for (size_t i = 0; i < targets.size(); ++i) {
            const Value& ov = targets[i].second[col];
            if (changed(i) && !ov.IsNull()) ix->tree->Remove(ix->KeyOf(ov), targets[i].first);
        }
        for (size_t i = 0; i < targets.size(); ++i) {
            const Value& nv = new_rows[i][col];
            if (changed(i) && !nv.IsNull()) ix->tree->Insert(ix->KeyOf(nv), new_rids[i]);
        }
    }
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
