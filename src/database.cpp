#include "database.h"

#include <algorithm>
#include <chrono>
#include <random>

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
    } catch (...) {
        Checkpoint();  // keep catalog statistics consistent with whatever was written
        throw;
    }
    // No WAL: we simply force everything to disk at the end of each modifying statement.
    Checkpoint();
    return r;
}

QueryResult Database::RunCreateTable(const CreateTableStmt& s) {
    std::vector<Column> cols;
    std::string pk;
    for (const auto& d : s.cols) {
        Column c{};
        c.name = d.name;
        c.type = d.type;
        c.not_null = d.not_null;
        cols.push_back(c);
        if (d.primary_key) {
            if (!pk.empty()) throw DbError("multiple primary keys for table \"" + s.name + "\" are not allowed");
            pk = d.name;
        }
        if ((d.primary_key || d.unique) && d.type != TypeId::INT)
            throw DbError("PRIMARY KEY / UNIQUE is only supported on INT columns");
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

QueryResult Database::RunDrop(const DropStmt& s) {
    if (s.index) {
        catalog_->DropIndex(s.name);
        return {{}, {}, "DROP INDEX"};
    }
    catalog_->DropTable(s.name);
    return {{}, {}, "DROP TABLE"};
}

void Database::InsertRow(TableInfo* t, Row row) {
    const Schema& schema = t->schema;
    for (size_t i = 0; i < schema.Size(); ++i) {
        row[i] = row[i].CastTo(schema.At(i).type);
        if (row[i].IsNull() && schema.At(i).not_null)
            throw DbError("null value in column \"" + schema.At(i).name + "\" violates not-null constraint");
    }
    auto indexes = catalog_->IndexesOf(t->name);
    for (IndexInfo* idx : indexes) {  // check constraints before touching anything
        const Value& v = row[static_cast<size_t>(idx->col_idx)];
        if (idx->unique && !v.IsNull() && idx->tree->ContainsKey(v.AsInt()))
            throw DbError("duplicate key value violates unique constraint \"" + idx->name + "\": " + idx->column +
                          "=" + v.ToString());
    }
    RID rid = t->heap->Insert(SerializeTuple(schema, row));
    for (IndexInfo* idx : indexes) {
        const Value& v = row[static_cast<size_t>(idx->col_idx)];
        if (!v.IsNull()) idx->tree->Insert(v.AsInt(), rid);
    }
    ++t->row_count;
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
            for (const auto& e : exprs) r.push_back(Evaluate(*BindExpr(e, empty), {}));
            source.push_back(std::move(r));
        }
    }

    for (const auto& src : source) {
        if (src.size() != positions.size())
            throw DbError("INSERT has " + std::to_string(src.size()) + " values but " +
                          std::to_string(positions.size()) + " target columns");
        Row full(schema.Size());
        for (size_t i = 0; i < positions.size(); ++i) full[positions[i]] = src[i];
        InsertRow(t, std::move(full));
    }
    return {{}, {}, "INSERT " + std::to_string(source.size())};
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
            if (!v.IsNull()) idx->tree->Remove(v.AsInt(), rid);
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
        if (ContainsAggregate(*e)) throw DbError("aggregate functions are not allowed in UPDATE");
        sets.emplace_back(static_cast<size_t>(idx), BindExpr(e, schema));
    }

    auto scan = planner_->PlanTableScan(t, s.where);
    std::vector<std::pair<RID, Row>> targets;
    scan->Init();
    Row row;
    while (scan->Next(&row)) targets.emplace_back(scan->CurrentRid(), row);

    auto indexes = catalog_->IndexesOf(t->name);
    for (auto& [rid, old_row] : targets) {
        Row new_row = old_row;
        for (auto& [idx, e] : sets) {
            new_row[idx] = Evaluate(*e, old_row).CastTo(schema.At(idx).type);
            if (new_row[idx].IsNull() && schema.At(idx).not_null)
                throw DbError("null value in column \"" + schema.At(idx).name + "\" violates not-null constraint");
        }
        for (IndexInfo* ix : indexes) {
            const Value& nv = new_row[static_cast<size_t>(ix->col_idx)];
            if (ix->unique && !nv.IsNull() && ix->tree->ContainsKey(nv.AsInt(), rid))
                throw DbError("duplicate key value violates unique constraint \"" + ix->name + "\": " + ix->column +
                              "=" + nv.ToString());
        }
        RID new_rid = t->heap->Update(rid, SerializeTuple(schema, new_row));
        for (IndexInfo* ix : indexes) {
            const Value& ov = old_row[static_cast<size_t>(ix->col_idx)];
            const Value& nv = new_row[static_cast<size_t>(ix->col_idx)];
            bool same = new_rid == rid && ov.IsNull() == nv.IsNull() && (ov.IsNull() || ov.AsInt() == nv.AsInt());
            if (same) continue;
            if (!ov.IsNull()) ix->tree->Remove(ov.AsInt(), rid);
            if (!nv.IsNull()) ix->tree->Insert(nv.AsInt(), new_rid);
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

    // Columns with a unique index (and the first INT column) get sequential ids.
    std::vector<bool> sequential(schema.Size(), false);
    for (IndexInfo* idx : catalog_->IndexesOf(table))
        if (idx->unique) sequential[static_cast<size_t>(idx->col_idx)] = true;
    if (!schema.Columns().empty() && schema.At(0).type == TypeId::INT) sequential[0] = true;
    std::vector<int64_t> next_id(schema.Size(), 1);
    {
        auto it = t->heap->Begin();
        RID rid;
        std::string buf;
        while (it.Next(&rid, &buf)) {
            Row r = DeserializeTuple(schema, buf.data(), buf.size());
            for (size_t i = 0; i < schema.Size(); ++i)
                if (sequential[i] && !r[i].IsNull()) next_id[i] = std::max(next_id[i], r[i].AsInt() + 1);
        }
    }

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
                    r.push_back(Value::Text(std::string(words[rng() % 16]) + "_" + std::to_string(rng() % 1000)));
                    break;
                case TypeId::NULL_T: r.push_back(Value::Null()); break;
            }
        }
        InsertRow(t, std::move(r));
    }
    Checkpoint();
    return {{}, {}, "INSERT " + std::to_string(n)};
}

}  // namespace minidb
