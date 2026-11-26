#include "catalog.h"

#include <algorithm>
#include <set>

namespace minidb {

namespace {

const char* kMagic = "MINIDB01";

struct Writer {
    std::string buf;
    void U8(uint8_t v) { buf.push_back(static_cast<char>(v)); }
    void U32(uint32_t v) { buf.append(reinterpret_cast<const char*>(&v), 4); }
    void I32(int32_t v) { buf.append(reinterpret_cast<const char*>(&v), 4); }
    void I64(int64_t v) { buf.append(reinterpret_cast<const char*>(&v), 8); }
    void Str(const std::string& s) {
        U32(static_cast<uint32_t>(s.size()));
        buf.append(s);
    }
};

struct Reader {
    const std::string& buf;
    size_t pos = 0;
    void Need(size_t n) {
        if (pos + n > buf.size()) throw DbError("catalog is corrupt");
    }
    uint8_t U8() {
        Need(1);
        return static_cast<uint8_t>(buf[pos++]);
    }
    uint32_t U32() {
        Need(4);
        auto v = ReadAs<uint32_t>(buf.data() + pos);
        pos += 4;
        return v;
    }
    int32_t I32() {
        Need(4);
        auto v = ReadAs<int32_t>(buf.data() + pos);
        pos += 4;
        return v;
    }
    int64_t I64() {
        Need(8);
        auto v = ReadAs<int64_t>(buf.data() + pos);
        pos += 8;
        return v;
    }
    std::string Str() {
        uint32_t n = U32();
        Need(n);
        std::string s = buf.substr(pos, n);
        pos += n;
        return s;
    }
};

// Catalog page layout: [next page i32][total length u32 (page 0 only)][bytes...]
constexpr size_t CAT_HDR = 8;
constexpr size_t CAT_CAP = PAGE_SIZE - CAT_HDR;

// A Value in the catalog: [type u8][payload] (INT / DOUBLE 8 bytes, BOOL 1, TEXT string).
void WriteValue(Writer& w, const Value& v) {
    w.U8(static_cast<uint8_t>(v.Type()));
    switch (v.Type()) {
        case TypeId::INT: w.I64(v.AsInt()); break;
        case TypeId::DOUBLE: {
            double d = v.AsDouble();
            int64_t bits;
            std::memcpy(&bits, &d, sizeof bits);
            w.I64(bits);
            break;
        }
        case TypeId::BOOL: w.U8(v.AsBool() ? 1 : 0); break;
        case TypeId::TEXT: w.Str(v.AsText()); break;
        case TypeId::DATE: w.I64(v.AsDays()); break;
        case TypeId::TIMESTAMP: w.I64(v.AsSeconds()); break;
        case TypeId::NULL_T: break;
    }
}

Value ReadValue(Reader& r) {
    switch (static_cast<TypeId>(r.U8())) {
        case TypeId::INT: return Value::Int(r.I64());
        case TypeId::DOUBLE: {
            int64_t bits = r.I64();
            double d;
            std::memcpy(&d, &bits, sizeof d);
            return Value::Double(d);
        }
        case TypeId::BOOL: return Value::Bool(r.U8() != 0);
        case TypeId::TEXT: return Value::Text(r.Str());
        case TypeId::DATE: return Value::Date(r.I64());
        case TypeId::TIMESTAMP: return Value::Timestamp(r.I64());
        case TypeId::NULL_T: return Value::Null();
    }
    throw DbError("catalog is corrupt (bad value type)");
}

}  // namespace

Catalog::Catalog(BufferPoolManager* bpm) : bpm_(bpm) {
    if (bpm_->Disk()->NumPages() == 0) {
        page_id_t pid;
        PageGuard g = PageGuard::New(bpm_, &pid);
        if (pid != CATALOG_PAGE_ID) throw DbError("internal: catalog must be page 0");
        WriteAs<page_id_t>(g.Data(), INVALID_PAGE_ID);
        g.MarkDirty();
        g.Release();
        Save();
        // Persist the empty catalog right away. Otherwise a crash before the first
        // write statement leaves an all-zero page 0, and the file can't be opened.
        bpm_->FlushPage(CATALOG_PAGE_ID);
    } else if (bpm_->Disk()->NumPages() == 1 && IsUninitializedCatalogPage()) {
        // Such a file can still exist (it was created by a version without the
        // flush above): it never held any data, so it is simply an empty database.
        {
            PageGuard g = PageGuard::Fetch(bpm_, CATALOG_PAGE_ID);
            WriteAs<page_id_t>(g.Data(), INVALID_PAGE_ID);
            g.MarkDirty();
        }
        Save();
        bpm_->FlushPage(CATALOG_PAGE_ID);
    } else {
        Load();
    }
}

bool Catalog::IsUninitializedCatalogPage() {
    PageGuard g = PageGuard::Fetch(bpm_, CATALOG_PAGE_ID);
    for (size_t i = 0; i < PAGE_SIZE; ++i)
        if (g.Data()[i] != 0) return false;
    return true;
}

void Catalog::Save() {
    std::string data = Serialize();
    // Make the chain long enough *before* writing. Growing it may take a page off
    // the free-page list, which is part of the serialized data, so serialize again
    // afterwards (the list only shrinks, so the data still fits).
    size_t needed = std::max<size_t>(1, (data.size() + CAT_CAP - 1) / CAT_CAP);
    bool grew = false;
    page_id_t pid = CATALOG_PAGE_ID;
    for (size_t i = 1; i < needed; ++i) {
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        page_id_t next = ReadAs<page_id_t>(g.Data());
        if (next == INVALID_PAGE_ID) {
            PageGuard ng = PageGuard::New(bpm_, &next);
            WriteAs<page_id_t>(ng.Data(), INVALID_PAGE_ID);
            ng.MarkDirty();
            WriteAs<page_id_t>(g.Data(), next);
            g.MarkDirty();
            grew = true;
        }
        pid = next;
    }
    if (grew) data = Serialize();

    size_t off = 0;
    pid = CATALOG_PAGE_ID;
    while (true) {
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        char* d = g.Data();
        size_t chunk = std::min(CAT_CAP, data.size() - off);
        if (pid == CATALOG_PAGE_ID) WriteAs<uint32_t>(d + 4, static_cast<uint32_t>(data.size()));
        std::memcpy(d + CAT_HDR, data.data() + off, chunk);
        off += chunk;
        g.MarkDirty();
        if (off >= data.size()) break;
        pid = ReadAs<page_id_t>(d);
        if (pid == INVALID_PAGE_ID) throw DbError("internal: catalog chain too short");
    }
}

std::string Catalog::Serialize() const {
    Writer w;
    w.Str(kMagic);
    w.U32(static_cast<uint32_t>(tables_.size()));
    for (auto& [name, t] : tables_) {
        w.Str(t->name);
        w.U32(static_cast<uint32_t>(t->schema.Size()));
        for (const auto& c : t->schema.Columns()) {
            w.Str(c.name);
            w.U8(static_cast<uint8_t>(c.type));
            w.U8(c.not_null ? 1 : 0);
        }
        w.I32(t->first_page);
        w.I64(t->row_count);
    }
    w.U32(static_cast<uint32_t>(indexes_.size()));
    for (auto& [name, idx] : indexes_) {
        w.Str(idx->name);
        w.Str(idx->table);
        w.Str(idx->columns[0]);  // the leading column; the others are in the last section
        w.U8(idx->unique ? 1 : 0);
        w.I32(idx->tree->Root());
    }
    // Free-page list (pages of dropped tables/indexes). Files written before this
    // section existed simply end here; Load treats that as "no free pages".
    w.U32(static_cast<uint32_t>(bpm_->FreePages().size()));
    for (page_id_t p : bpm_->FreePages()) w.I32(p);
    // Column DEFAULT values: [count u32] then (table, column index, value) each.
    // Also appended last, so older files (without the section) still load.
    uint32_t ndefaults = 0;
    for (auto& [name, t] : tables_)
        for (const auto& c : t->schema.Columns()) ndefaults += c.default_value ? 1 : 0;
    w.U32(ndefaults);
    for (auto& [name, t] : tables_) {
        for (size_t i = 0; i < t->schema.Size(); ++i) {
            const Column& c = t->schema.At(i);
            if (!c.default_value) continue;
            w.Str(t->name);
            w.U32(static_cast<uint32_t>(i));
            WriteValue(w, *c.default_value);
        }
    }
    // Multi-column indexes: [count u32] then (index name, [ncols u32] column names)
    // each, with all the columns (the leading one again). Appended last as well:
    // an index missing here (every index, in older files) has just one column.
    uint32_t ncomposite = 0;
    for (auto& [name, idx] : indexes_) ncomposite += idx->Composite() ? 1 : 0;
    w.U32(ncomposite);
    for (auto& [name, idx] : indexes_) {
        if (!idx->Composite()) continue;
        w.Str(idx->name);
        w.U32(static_cast<uint32_t>(idx->columns.size()));
        for (const auto& c : idx->columns) w.Str(c);
    }
    // Views: [count u32] then (name, sql, [column count u32], column names) each.
    // Appended last as well, so older files (without views) still load.
    w.U32(static_cast<uint32_t>(views_.size()));
    for (auto& [name, v] : views_) {
        w.Str(v->name);
        w.Str(v->sql);
        w.U32(static_cast<uint32_t>(v->columns.size()));
        for (const auto& c : v->columns) w.Str(c);
    }
    // Foreign keys: [count u32] then (name, table, column, ref_table, ref_column,
    // on_delete u8, on_update u8) each. Appended last as well.
    w.U32(static_cast<uint32_t>(foreign_keys_.size()));
    for (auto& [name, fk] : foreign_keys_) {
        w.Str(fk->name);
        w.Str(fk->table);
        w.Str(fk->column);
        w.Str(fk->ref_table);
        w.Str(fk->ref_column);
        w.U8(static_cast<uint8_t>(fk->on_delete));
        w.U8(static_cast<uint8_t>(fk->on_update));
    }
    // Column statistics (ANALYZE): [count u32] then (table, rows i64, [ncols u32],
    // per column: nulls i64, distinct i64, [nmcv u32] (value, count i64) each,
    // [nhist u32] values). Appended last as well.
    uint32_t nstats = 0;
    for (auto& [name, t] : tables_) nstats += t->stats.empty() ? 0 : 1;
    w.U32(nstats);
    for (auto& [name, t] : tables_) {
        if (t->stats.empty()) continue;
        w.Str(t->name);
        w.I64(t->stats[0].rows);
        w.U32(static_cast<uint32_t>(t->stats.size()));
        for (const ColumnStats& s : t->stats) {
            w.I64(s.nulls);
            w.I64(s.distinct);
            w.U32(static_cast<uint32_t>(s.mcv.size()));
            for (size_t i = 0; i < s.mcv.size(); ++i) {
                WriteValue(w, s.mcv[i]);
                w.I64(s.mcv_counts[i]);
            }
            w.U32(static_cast<uint32_t>(s.histogram.size()));
            for (const Value& v : s.histogram) WriteValue(w, v);
        }
    }
    return std::move(w.buf);
}

void Catalog::Reload() {
    views_.clear();
    foreign_keys_.clear();
    indexes_.clear();
    tables_.clear();
    Load();
}

void Catalog::Load() {
    std::string data;
    size_t total;
    page_id_t pid = CATALOG_PAGE_ID;
    {
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        total = ReadAs<uint32_t>(g.Data() + 4);
    }
    while (data.size() < total) {
        if (pid == INVALID_PAGE_ID) throw DbError("catalog is corrupt (chain too short)");
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        size_t chunk = std::min(CAT_CAP, total - data.size());
        data.append(g.Data() + CAT_HDR, chunk);
        pid = ReadAs<page_id_t>(g.Data());
    }

    Reader r{data};
    if (r.Str() != kMagic) throw DbError("not a MiniDB database file");
    uint32_t nt = r.U32();
    for (uint32_t i = 0; i < nt; ++i) {
        auto t = std::make_unique<TableInfo>();
        t->name = r.Str();
        uint32_t nc = r.U32();
        std::vector<Column> cols;
        for (uint32_t j = 0; j < nc; ++j) {
            Column c;
            c.name = r.Str();
            uint8_t type = r.U8();
            if (type == 0 || type > kMaxTypeId) throw DbError("catalog is corrupt (bad column type)");
            c.type = static_cast<TypeId>(type);
            c.not_null = r.U8() != 0;
            c.table = t->name;
            cols.push_back(c);
        }
        t->schema = Schema(std::move(cols));
        t->first_page = r.I32();
        t->row_count = r.I64();
        t->heap = std::make_unique<TableHeap>(bpm_, t->first_page);
        tables_[t->name] = std::move(t);
    }
    // The trees are opened once all sections are read: whether a tree is wide
    // depends on the index's column count, which the last section may change.
    std::map<std::string, page_id_t> roots;
    uint32_t ni = r.U32();
    for (uint32_t i = 0; i < ni; ++i) {
        auto idx = std::make_unique<IndexInfo>();
        idx->name = r.Str();
        idx->table = r.Str();
        idx->columns = {r.Str()};
        idx->unique = r.U8() != 0;
        roots[idx->name] = r.I32();
        RequireTable(idx->table);
        indexes_[idx->name] = std::move(idx);
    }
    std::set<page_id_t> free_pages;
    if (r.pos < data.size()) {  // absent in files from before page reuse existed
        uint32_t nf = r.U32();
        for (uint32_t i = 0; i < nf; ++i) {
            page_id_t p = r.I32();
            if (p <= CATALOG_PAGE_ID || p >= bpm_->Disk()->NumPages()) throw DbError("catalog is corrupt");
            free_pages.insert(p);
        }
    }
    bpm_->SetFreePages(std::move(free_pages));
    if (r.pos < data.size()) {  // absent in files from before DEFAULT existed
        uint32_t nd = r.U32();
        for (uint32_t i = 0; i < nd; ++i) {
            TableInfo* t = RequireTable(r.Str());
            uint32_t col = r.U32();
            Value v = ReadValue(r);
            if (col >= t->schema.Size()) throw DbError("catalog is corrupt (bad DEFAULT)");
            std::vector<Column> cols = t->schema.Columns();
            cols[col].default_value = std::move(v);
            t->schema = Schema(std::move(cols));
        }
    }
    if (r.pos < data.size()) {  // absent in files from before multi-column indexes existed
        uint32_t nm = r.U32();
        for (uint32_t i = 0; i < nm; ++i) {
            auto it = indexes_.find(r.Str());
            uint32_t nc = r.U32();
            if (it == indexes_.end() || nc < 2 || nc > IndexInfo::kMaxColumns)
                throw DbError("catalog is corrupt (bad multi-column index)");
            std::vector<std::string> cols;
            for (uint32_t j = 0; j < nc; ++j) cols.push_back(r.Str());
            if (cols[0] != it->second->columns[0]) throw DbError("catalog is corrupt (bad multi-column index)");
            it->second->columns = std::move(cols);
        }
    }
    if (r.pos < data.size()) {  // absent in files from before views existed
        uint32_t nv = r.U32();
        for (uint32_t i = 0; i < nv; ++i) {
            auto v = std::make_unique<ViewInfo>();
            v->name = r.Str();
            v->sql = r.Str();
            uint32_t nc = r.U32();
            for (uint32_t j = 0; j < nc; ++j) v->columns.push_back(r.Str());
            if (tables_.count(v->name) || views_.count(v->name)) throw DbError("catalog is corrupt (duplicate view)");
            views_[v->name] = std::move(v);
        }
    }
    if (r.pos < data.size()) {  // absent in files from before foreign keys existed
        uint32_t nfk = r.U32();
        auto read_action = [&r] {
            uint8_t a = r.U8();
            if (a > static_cast<uint8_t>(FkAction::SET_DEFAULT)) throw DbError("catalog is corrupt (bad foreign key)");
            return static_cast<FkAction>(a);
        };
        for (uint32_t i = 0; i < nfk; ++i) {
            auto fk = std::make_unique<ForeignKey>();
            fk->name = r.Str();
            fk->table = r.Str();
            fk->column = r.Str();
            fk->ref_table = r.Str();
            fk->ref_column = r.Str();
            fk->on_delete = read_action();
            fk->on_update = read_action();
            if (RequireTable(fk->table)->schema.Find("", fk->column) < 0 ||
                RequireTable(fk->ref_table)->schema.Find("", fk->ref_column) < 0 ||
                !UniqueIndexOn(fk->ref_table, fk->ref_column))
                throw DbError("catalog is corrupt (bad foreign key)");
            std::string name = fk->name;
            foreign_keys_[name] = std::move(fk);
        }
    }
    if (r.pos < data.size()) {  // absent in files from before ANALYZE existed
        uint32_t ns = r.U32();
        for (uint32_t i = 0; i < ns; ++i) {
            TableInfo* t = RequireTable(r.Str());
            const int64_t rows = r.I64();
            const uint32_t nc = r.U32();
            if (nc != t->schema.Size() || rows < 0) throw DbError("catalog is corrupt (bad statistics)");
            std::vector<ColumnStats> stats(nc);
            for (ColumnStats& s : stats) {
                s.rows = rows;
                s.nulls = r.I64();
                s.distinct = r.I64();
                uint32_t nm = r.U32();
                if (nm > ColumnStats::kMaxMcv) throw DbError("catalog is corrupt (bad statistics)");
                for (uint32_t j = 0; j < nm; ++j) {
                    s.mcv.push_back(ReadValue(r));
                    s.mcv_counts.push_back(r.I64());
                }
                uint32_t nh = r.U32();
                if (nh > ColumnStats::kBuckets + 1) throw DbError("catalog is corrupt (bad statistics)");
                for (uint32_t j = 0; j < nh; ++j) s.histogram.push_back(ReadValue(r));
            }
            t->stats = std::move(stats);
        }
    }
    for (auto& [name, idx] : indexes_) {
        try {
            ResolveIndexColumns(
idx.get(), RequireTable(idx->table)->schema);
        } catch (const DbError&) {
            throw DbError("catalog is corrupt (index \"" + name + "\" on unknown or unindexable columns)");
        }
        idx->tree = std::make_unique<BPlusTree>(bpm_, roots.at(name), BPlusTree::LEAF_CAPACITY,
                                                BPlusTree::INTERNAL_CAPACITY, idx->Composite());
    }
}

// Looks up the index's columns in `schema` and sets the fields derived from them.
// Throws (leaving `idx` unusable) for an unknown, repeated or unindexable column.
void Catalog::ResolveIndexColumns(IndexInfo* idx, const Schema& schema) {
    if (idx->columns.empty() || idx->columns.size() > IndexInfo::kMaxColumns)
        throw DbError("an index must have between 1 and " + std::to_string(IndexInfo::kMaxColumns) + " columns");
    idx->col_idxs.clear();
    std::set<std::string> seen;
    for (const auto& name : idx->columns) {
        if (!seen.insert(name).second) throw DbError("column \"" + name + "\" appears twice in index \"" + idx->name + "\"");
        int col = schema.Find("", name);
        if (col < 0) throw DbError("column \"" + name + "\" does not exist in table \"" + idx->table + "\"");
        TypeId type = schema.At(static_cast<size_t>(col)).type;
        if (!IsIndexable(type))
            throw DbError("indexes are only supported on INT, TEXT, DATE and TIMESTAMP columns (\"" + name + "\" is " + TypeName(type) +
                          ")");
        idx->col_idxs.push_back(col);
    }
    auto type_of = [&](size_t i) { return schema.At(static_cast<size_t>(idx->col_idxs[i])).type; };
    idx->hashed = type_of(0) == TypeId::TEXT;
    idx->second_is_text = idx->columns.size() > 1 && type_of(1) == TypeId::TEXT;
}

TableInfo* Catalog::CreateTable(const std::string& name, std::vector<Column> cols) {
    if (tables_.count(name)) throw DbError("table \"" + name + "\" already exists");
    if (views_.count(name)) throw DbError("a view named \"" + name + "\" already exists");
    if (cols.empty()) throw DbError("a table must have at least one column");
    std::set<std::string> seen;
    for (auto& c : cols) {
        if (!seen.insert(c.name).second) throw DbError("column \"" + c.name + "\" specified more than once");
        c.table = name;
    }
    auto t = std::make_unique<TableInfo>();
    t->name = name;
    t->schema = Schema(std::move(cols));
    t->first_page = TableHeap::Create(bpm_);
    t->heap = std::make_unique<TableHeap>(bpm_, t->first_page);
    TableInfo* raw = t.get();
    tables_[name] = std::move(t);
    return raw;
}

TableInfo* Catalog::GetTable(const std::string& name) {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : it->second.get();
}

TableInfo* Catalog::RequireTable(const std::string& name) {
    TableInfo* t = GetTable(name);
    if (!t) {
        // Views are read-only: INSERT / UPDATE / DELETE / ALTER / CREATE INDEX /
        // DROP TABLE on one all end up here.
        if (views_.count(name)) throw DbError("\"" + name + "\" is a view, not a table");
        throw DbError("table \"" + name + "\" does not exist");
    }
    return t;
}

void Catalog::CreateView(const std::string& name, std::vector<std::string> columns, std::string sql, bool replace) {
    if (tables_.count(name)) throw DbError("a table named \"" + name + "\" already exists");
    if (views_.count(name) && !replace) throw DbError("view \"" + name + "\" already exists");
    std::set<std::string> seen;
    for (const auto& c : columns)
        if (!seen.insert(c).second) throw DbError("column \"" + c + "\" specified more than once");
    auto v = std::make_unique<ViewInfo>();
    v->name = name;
    v->columns = std::move(columns);
    v->sql = std::move(sql);
    views_[name] = std::move(v);
}

ViewInfo* Catalog::GetView(const std::string& name) {
    auto it = views_.find(name);
    return it == views_.end() ? nullptr : it->second.get();
}

void Catalog::DropView(const std::string& name) {
    if (views_.erase(name)) return;
    if (tables_.count(name)) throw DbError("\"" + name + "\" is a table, not a view (use DROP TABLE)");
    throw DbError("view \"" + name + "\" does not exist");
}

std::vector<ViewInfo*> Catalog::Views() {
    std::vector<ViewInfo*> out;
    for (auto& [n, v] : views_) out.push_back(v.get());
    return out;
}

void Catalog::DropTable(const std::string& name) {
    TableInfo* t = RequireTable(name);
    for (ForeignKey* fk : ForeignKeysTo(name))
        if (fk->table != name)
            throw DbError("cannot drop table \"" + name + "\" because foreign key constraint \"" + fk->name +
                          "\" on table \"" + fk->table + "\" references it");
    for (ForeignKey* fk : ForeignKeysOf(name)) {
        std::string fk_name = fk->name;  // erase destroys *fk
        foreign_keys_.erase(fk_name);
    }
    std::vector<page_id_t> pages = t->heap->Pages();
    for (auto it = indexes_.begin(); it != indexes_.end();) {
        if (it->second->table == name) {
            auto tree_pages = it->second->tree->Pages();
            pages.insert(pages.end(), tree_pages.begin(), tree_pages.end());
            it = indexes_.erase(it);
        } else {
            ++it;
        }
    }
    tables_.erase(name);
    FreePages(pages);  // reused by the next tables / indexes instead of growing the file
}

void Catalog::FreePages(const std::vector<page_id_t>& pages) {
    for (page_id_t p : pages) bpm_->DeletePage(p);
}

void Catalog::RenameTable(const std::string& name, const std::string& new_name) {
    TableInfo* t = RequireTable(name);
    if (tables_.count(new_name)) throw DbError("table \"" + new_name + "\" already exists");
    if (views_.count(new_name)) throw DbError("a view named \"" + new_name + "\" already exists");
    std::vector<Column> cols = t->schema.Columns();
    for (auto& c : cols) c.table = new_name;
    t->schema = Schema(std::move(cols));
    t->name = new_name;
    for (auto& [n, idx] : indexes_)
        if (idx->table == name) idx->table = new_name;
    for (auto& [n, fk] : foreign_keys_) {
        if (fk->table == name) fk->table = new_name;
        if (fk->ref_table == name) fk->ref_table = new_name;
    }
    auto node = tables_.extract(name);
    node.key() = new_name;
    tables_.insert(std::move(node));
    // The primary key is recognized by its index name, so it moves along.
    if (indexes_.count(name + "_pkey") && !indexes_.count(new_name + "_pkey")) {
        auto idx = indexes_.extract(name + "_pkey");
        idx.key() = new_name + "_pkey";
        idx.mapped()->name = new_name + "_pkey";
        indexes_.insert(std::move(idx));
    }
}

void Catalog::RenameColumn(const std::string& table, const std::string& column, const std::string& new_name) {
    TableInfo* t = RequireTable(table);
    int col = t->schema.Find("", column);
    if (col < 0) throw DbError("column \"" + column + "\" does not exist in table \"" + table + "\"");
    if (t->schema.Find("", new_name) >= 0)
        throw DbError("column \"" + new_name + "\" of relation \"" + table + "\" already exists");
    std::vector<Column> cols = t->schema.Columns();
    cols[static_cast<size_t>(col)].name = new_name;
    t->schema = Schema(std::move(cols));
    for (auto& [n, idx] : indexes_)
        if (idx->table == table)
            for (auto& c : idx->columns)
                if (c == column) c = new_name;
    for (auto& [n, fk] : foreign_keys_) {
        if (fk->table == table && fk->column == column) fk->column = new_name;
        if (fk->ref_table == table && fk->ref_column == column) fk->ref_column = new_name;
    }
}

void Catalog::RewriteTable(TableInfo* t, std::vector<Column> cols, const std::function<Row(Row)>& convert) {
    if (cols.empty()) throw DbError("a table must have at least one column");
    for (auto& c : cols) c.table = t->name;
    Schema new_schema(std::move(cols));

    // Pass 1: convert and check every row; nothing is modified yet.
    {
        auto it = t->heap->Begin();
        RID rid;
        std::string buf;
        while (it.Next(&rid, &buf)) {
            Row row = convert(DeserializeTuple(t->schema, buf.data(), buf.size()));
            if (SerializeTuple(new_schema, row).size() > TablePage::MAX_TUPLE)
                throw DbError("a row would become too large (max " + std::to_string(TablePage::MAX_TUPLE) + " bytes)");
        }
    }

    // Pass 2: drop the indexes, copy the rows into a new heap, swap, free the old pages.
    // An index survives only if all of its columns do (as in PostgreSQL, dropping
    // one column of a multi-column index drops the whole index).
    struct IndexDef {
        std::string name;
        std::vector<std::string> columns;
        bool unique;
    };
    std::vector<IndexDef> keep;
    for (IndexInfo* idx : IndexesOf(t->name)) {
        bool all_kept = std::all_of(idx->columns.begin(), idx->columns.end(),
                                    [&](const std::string& c) { return new_schema.Find("", c) >= 0; });
        if (all_kept) keep.push_back({idx->name, idx->columns, idx->unique});
        std::string name = idx->name;  // DropIndex destroys *idx
        DropIndexUnchecked(name);      // recreated below, so foreign keys keep their index
    }
    page_id_t first = TableHeap::Create(bpm_);
    auto heap = std::make_unique<TableHeap>(bpm_, first);
    {
        auto it = t->heap->Begin();
        RID rid;
        std::string buf;
        while (it.Next(&rid, &buf))
            heap->Insert(SerializeTuple(new_schema, convert(DeserializeTuple(t->schema, buf.data(), buf.size()))));
    }
    std::vector<page_id_t> old_pages = t->heap->Pages();
    t->schema = std::move(new_schema);
    t->first_page = first;
    t->heap = std::move(heap);
    FreePages(old_pages);
    t->stats.clear();  // the columns changed
    for (const auto& d : keep) CreateIndex(d.name, t->name, d.columns, d.unique);
}

IndexInfo* Catalog::CreateIndex(const std::string& name, const std::string& table,
                                const std::vector<std::string>& columns, bool unique) {
    if (indexes_.count(name)) throw DbError("index \"" + name + "\" already exists");
    TableInfo* t = RequireTable(table);

    auto idx = std::make_unique<IndexInfo>();
    idx->name = name;
    idx->table = table;
    idx->columns = columns;
    idx->unique = unique;
    ResolveIndexColumns(idx.get(), t->schema);
    idx->tree = std::make_unique<BPlusTree>(bpm_, INVALID_PAGE_ID, BPlusTree::LEAF_CAPACITY,
                                            BPlusTree::INTERNAL_CAPACITY, idx->Composite());

    // Bulk-build from the existing rows.
    auto it = t->heap->Begin();
    RID rid;
    std::string buf;
    while (it.Next(&rid, &buf)) {
        Row row = DeserializeTuple(t->schema, buf.data(), buf.size());
        if (!idx->Indexes(row)) continue;
        Row key = idx->KeyValues(row);
        if (unique && KeyTaken(idx.get(), key)) {
            FreePages(idx->tree->Pages());  // don't leak the partially built tree
            throw DbError("could not create unique index \"" + name + "\": duplicate key " +
                          (idx->Composite() ? idx->Describe(key) : key[0].ToString()));
        }
        idx->tree->Insert(idx->EntryFor(key, rid));
    }
    IndexInfo* raw = idx.get();
    indexes_[name] = std::move(idx);
    return raw;
}

void Catalog::DropIndex(const std::string& name) {
    auto it = indexes_.find(name);
    if (it == indexes_.end()) throw DbError("index \"" + name + "\" does not exist");
    const IndexInfo* idx = it->second.get();
    if (idx->unique) {
        bool other_unique = false;
        for (auto& [n, other] : indexes_)
            if (n != name && other->unique && other->table == idx->table && other->columns == idx->columns)
                other_unique = true;
        if (!other_unique && !idx->Composite())
            for (ForeignKey* fk : ForeignKeysTo(idx->table))
                if (fk->ref_column == idx->columns[0])
                    throw DbError("cannot drop index \"" + name + "\" because foreign key constraint \"" + fk->name +
                                  "\" on table \"" + fk->table + "\" requires it");
    }
    DropIndexUnchecked(name);
}

void Catalog::DropIndexUnchecked(const std::string& name) {
    auto it = indexes_.find(name);
    if (it == indexes_.end()) throw DbError("index \"" + name + "\" does not exist");
    std::vector<page_id_t> pages = it->second->tree->Pages();
    indexes_.erase(it);
    FreePages(pages);
}

int64_t HashTextKey(const std::string& s) {
    uint64_t h = 14695981039346656037ull;  // FNV-1a, 64 bit
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return static_cast<int64_t>(h);
}

int64_t IndexKeyPart(const Value& v) {
    switch (v.Type()) {
        case TypeId::INT: return v.AsInt();
        case TypeId::TEXT: return HashTextKey(v.AsText());
        case TypeId::DATE: return v.AsDays();
        case TypeId::TIMESTAMP: return v.AsSeconds();
        default: throw DbError(std::string("internal: cannot index a ") + TypeName(v.Type()) + " value");
    }
}

int64_t IndexKey2(const Value* rest, size_t n) {
    if (n == 1) return rest[0].IsNull() ? IndexInfo::kNullKey2 : IndexKeyPart(rest[0]);
    std::string enc;  // the type-tagged encoding keeps (1, 'a') and ('1', 'a') etc. apart
    for (size_t i = 0; i < n; ++i) rest[i].AppendKey(&enc);
    return HashTextKey(enc);
}

Row IndexInfo::KeyValues(const Row& row) const {
    Row out;
    out.reserve(col_idxs.size());
    for (int c : col_idxs) out.push_back(row[static_cast<size_t>(c)]);
    return out;
}

IndexKey IndexInfo::EntryFor(const Row& key_values, RID rid) const {
    IndexKey k{IndexKeyPart(key_values[0]), rid};
    if (Composite()) k.key2 = IndexKey2(key_values.data() + 1, key_values.size() - 1);
    return k;
}

std::string IndexInfo::Describe(const Row& key_values) const {
    if (!Composite()) return columns[0] + "=" + key_values[0].ToString();
    std::string names, values;
    for (size_t i = 0; i < columns.size(); ++i) {
        names += (i ? ", " : "") + columns[i];
        values += (i ? ", " : "") + key_values[i].ToString();
    }
    return "(" + names + ")=(" + values + ")";
}

bool Catalog::KeyTaken(IndexInfo* idx, const Row& key_values, const std::function<bool(RID)>& ignore) {
    for (const Value& v : key_values)
        if (v.IsNull()) return false;  // NULL never equals anything, so never a duplicate
    TableInfo* t = idx->Exact() ? nullptr : RequireTable(idx->table);
    const IndexKey probe = idx->EntryFor(key_values, RID{});
    std::string buf;
    for (auto it = idx->tree->LowerBound(probe.key, probe.key2);
         it.Valid() && it.Entry().key == probe.key && it.Entry().key2 == probe.key2; it.Next()) {
        RID rid = it.Entry().rid;
        if (ignore && ignore(rid)) continue;
        if (idx->Exact()) return true;
        // Same tree key (a hash, or key2 of a NULL): a conflict only if every value is equal.
        if (!t->heap->Get(rid, &buf)) continue;
        Row row = DeserializeTuple(t->schema, buf.data(), buf.size());
        bool equal = true;
        for (size_t i = 0; i < key_values.size() && equal; ++i) {
            const Value& other = row[static_cast<size_t>(idx->col_idxs[i])];
            equal = !other.IsNull() && other.Compare(key_values[i]) == 0;
        }
        if (equal) return true;
    }
    return false;
}

std::vector<IndexInfo*> Catalog::IndexesOf(const std::string& table) {
    std::vector<IndexInfo*> out;
    for (auto& [n, idx] : indexes_)
        if (idx->table == table) out.push_back(idx.get());
    return out;
}

std::vector<TableInfo*> Catalog::Tables() {
    std::vector<TableInfo*> out;
    for (auto& [n, t] : tables_) out.push_back(t.get());
    return out;
}

std::vector<IndexInfo*> Catalog::Indexes() {
    std::vector<IndexInfo*> out;
    for (auto& [n, i] : indexes_) out.push_back(i.get());
    return out;
}

IndexInfo* Catalog::PrimaryKeyIndex(const std::string& table) {
    auto it = indexes_.find(table + "_pkey");
    return it != indexes_.end() && it->second->table == table && it->second->unique ? it->second.get() : nullptr;
}

namespace {
bool OnlyOn(const IndexInfo* idx, const std::string& column) {
    return idx->columns.size() == 1 && idx->columns[0] == column;
}
}  // namespace

IndexInfo* Catalog::UniqueIndexOn(const std::string& table, const std::string& column) {
    IndexInfo* pk = PrimaryKeyIndex(table);
    if (pk && OnlyOn(pk, column)) return pk;
    for (auto& [n, idx] : indexes_)
        if (idx->unique && idx->table == table && OnlyOn(idx.get(), column)) return idx.get();
    return nullptr;
}

IndexInfo* Catalog::IndexOn(const std::string& table, const std::string& column) {
    if (IndexInfo* u = UniqueIndexOn(table, column)) return u;
    for (auto& [n, idx] : indexes_)
        if (idx->table == table && OnlyOn(idx.get(), column)) return idx.get();
    // A multi-column index whose leading column it is finds the rows too.
    for (auto& [n, idx] : indexes_)
        if (idx->table == table && idx->columns[0] == column) return idx.get();
    return nullptr;
}

void Catalog::ResolveForeignKey(ForeignKey* fk, TypeId child_type) {
    TableInfo* parent = GetTable(fk->ref_table);
    if (!parent) throw DbError("referenced table \"" + fk->ref_table + "\" does not exist");
    if (fk->ref_column.empty()) {
        IndexInfo* pk = PrimaryKeyIndex(fk->ref_table);
        if (!pk) throw DbError("there is no primary key for referenced table \"" + fk->ref_table + "\"");
        if (pk->Composite())
            throw DbError("the primary key of referenced table \"" + fk->ref_table +
                          "\" has several columns; a foreign key must reference a single column");
        fk->ref_column = pk->columns[0];
    }
    int col = parent->schema.Find("", fk->ref_column);
    if (col < 0)
        throw DbError("column \"" + fk->ref_column + "\" referenced in foreign key constraint does not exist in table \"" +
                      fk->ref_table + "\"");
    if (!UniqueIndexOn(fk->ref_table, fk->ref_column))
        throw DbError("there is no unique constraint on column \"" + fk->ref_column + "\" of referenced table \"" +
                      fk->ref_table + "\" (it must be the PRIMARY KEY or UNIQUE)");
    TypeId parent_type = parent->schema.At(static_cast<size_t>(col)).type;
    if (parent_type != child_type)
        throw DbError("foreign key columns \"" + fk->column + "\" and \"" + fk->ref_column +
                      "\" are of incompatible types: " + TypeName(child_type) + " and " + TypeName(parent_type));
}

ForeignKey* Catalog::CreateForeignKey(ForeignKey fk) {
    if (foreign_keys_.count(fk.name)) throw DbError("constraint \"" + fk.name + "\" already exists");
    TableInfo* child = RequireTable(fk.table);
    int col = child->schema.Find("", fk.column);
    if (col < 0)
        throw DbError("column \"" + fk.column + "\" referenced in foreign key constraint does not exist in table \"" +
                      fk.table + "\"");
    ResolveForeignKey(&fk, child->schema.At(static_cast<size_t>(col)).type);
    if (fk.on_delete == FkAction::SET_NULL || fk.on_update == FkAction::SET_NULL) {
        if (child->schema.At(static_cast<size_t>(col)).not_null)
            throw DbError("foreign key constraint \"" + fk.name + "\" cannot SET NULL: column \"" + fk.column +
                          "\" is NOT NULL");
    }

    // Every existing row must already satisfy the constraint.
    IndexInfo* parent_idx = UniqueIndexOn(fk.ref_table, fk.ref_column);
    auto it = child->heap->Begin();
    RID rid;
    std::string buf;
    while (it.Next(&rid, &buf)) {
        Row row = DeserializeTuple(child->schema, buf.data(), buf.size());
        const Value& v = row[static_cast<size_t>(col)];
        if (!v.IsNull() && !KeyTaken(parent_idx, v))
            throw DbError("foreign key constraint \"" + fk.name + "\" is violated by an existing row of table \"" +
                          fk.table + "\": key (" + fk.column + ")=(" + v.ToString() + ") is not present in table \"" +
                          fk.ref_table + "\"");
    }
    auto owned = std::make_unique<ForeignKey>(std::move(fk));
    ForeignKey* raw = owned.get();
    foreign_keys_[raw->name] = std::move(owned);
    return raw;
}

void Catalog::DropForeignKey(const std::string& name) {
    if (!foreign_keys_.erase(name)) throw DbError("constraint \"" + name + "\" does not exist");
}

ForeignKey* Catalog::GetForeignKey(const std::string& name) {
    auto it = foreign_keys_.find(name);
    return it == foreign_keys_.end() ? nullptr : it->second.get();
}

std::vector<ForeignKey*> Catalog::ForeignKeys() {
    std::vector<ForeignKey*> out;
    for (auto& [n, fk] : foreign_keys_) out.push_back(fk.get());
    return out;
}

std::vector<ForeignKey*> Catalog::ForeignKeysOf(const std::string& table) {
    std::vector<ForeignKey*> out;
    for (auto& [n, fk] : foreign_keys_)
        if (fk->table == table) out.push_back(fk.get());
    return out;
}

std::vector<ForeignKey*> Catalog::ForeignKeysTo(const std::string& table) {
    std::vector<ForeignKey*> out;
    for (auto& [n, fk] : foreign_keys_)
        if (fk->ref_table == table) out.push_back(fk.get());
    return out;
}

std::string Catalog::NewForeignKeyName(const std::string& table, const std::string& column) {
    std::string base = table + "_" + column + "_fkey";
    std::string name = base;
    for (int i = 1; foreign_keys_.count(name); ++i) name = base + std::to_string(i);
    return name;
}

void Catalog::Analyze(TableInfo* t) {
    auto it = t->heap->Begin();
    RID rid;
    std::string buf;
    t->stats = ComputeStats(t->schema, [&](Row* row) {
        if (!it.Next(&rid, &buf)) return false;
        *row = DeserializeTuple(t->schema, buf.data(), buf.size());
        return true;
    });
    t->row_count = t->stats.empty() ? t->row_count : t->stats[0].rows;  // exact again
}

}  // namespace minidb
