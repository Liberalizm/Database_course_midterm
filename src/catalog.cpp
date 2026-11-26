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
        w.Str(idx->column);
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
    // Views: [count u32] then (name, sql, [column count u32], column names) each.
    // Appended last as well, so older files (without views) still load.
    w.U32(static_cast<uint32_t>(views_.size()));
    for (auto& [name, v] : views_) {
        w.Str(v->name);
        w.Str(v->sql);
        w.U32(static_cast<uint32_t>(v->columns.size()));
        for (const auto& c : v->columns) w.Str(c);
    }
    return std::move(w.buf);
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
            c.type = static_cast<TypeId>(r.U8());
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
    uint32_t ni = r.U32();
    for (uint32_t i = 0; i < ni; ++i) {
        auto idx = std::make_unique<IndexInfo>();
        idx->name = r.Str();
        idx->table = r.Str();
        idx->column = r.Str();
        idx->unique = r.U8() != 0;
        page_id_t root = r.I32();
        TableInfo* t = RequireTable(idx->table);
        idx->col_idx = t->schema.Find("", idx->column);
        if (idx->col_idx < 0) throw DbError("catalog is corrupt (index on unknown column)");
        idx->hashed = t->schema.At(static_cast<size_t>(idx->col_idx)).type == TypeId::TEXT;
        idx->tree = std::make_unique<BPlusTree>(bpm_, root);
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
    auto node = tables_.extract(name);
    node.key() = new_name;
    tables_.insert(std::move(node));
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
        if (idx->table == table && idx->column == column) idx->column = new_name;
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
    struct IndexDef {
        std::string name, column;
        bool unique;
    };
    std::vector<IndexDef> keep;
    for (IndexInfo* idx : IndexesOf(t->name)) {
        if (new_schema.Find("", idx->column) >= 0) keep.push_back({idx->name, idx->column, idx->unique});
        std::string name = idx->name;  // DropIndex destroys *idx
        DropIndex(name);
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
    for (const auto& d : keep) CreateIndex(d.name, t->name, d.column, d.unique);
}

IndexInfo* Catalog::CreateIndex(const std::string& name, const std::string& table, const std::string& column,
                                bool unique) {
    if (indexes_.count(name)) throw DbError("index \"" + name + "\" already exists");
    TableInfo* t = RequireTable(table);
    int col = t->schema.Find("", column);
    if (col < 0) throw DbError("column \"" + column + "\" does not exist in table \"" + table + "\"");
    TypeId type = t->schema.At(col).type;
    if (type != TypeId::INT && type != TypeId::TEXT)
        throw DbError("indexes are only supported on INT and TEXT columns (\"" + column + "\" is " +
                      TypeName(type) + ")");

    auto idx = std::make_unique<IndexInfo>();
    idx->name = name;
    idx->table = table;
    idx->column = column;
    idx->col_idx = col;
    idx->unique = unique;
    idx->hashed = type == TypeId::TEXT;
    idx->tree = std::make_unique<BPlusTree>(bpm_, INVALID_PAGE_ID);

    // Bulk-build from the existing rows.
    auto it = t->heap->Begin();
    RID rid;
    std::string buf;
    while (it.Next(&rid, &buf)) {
        Row row = DeserializeTuple(t->schema, buf.data(), buf.size());
        const Value& v = row[col];
        if (v.IsNull()) continue;
        if (unique && KeyTaken(idx.get(), v)) {
            FreePages(idx->tree->Pages());  // don't leak the partially built tree
            throw DbError("could not create unique index \"" + name + "\": duplicate key " + v.ToString());
        }
        idx->tree->Insert(idx->KeyOf(v), rid);
    }
    IndexInfo* raw = idx.get();
    indexes_[name] = std::move(idx);
    return raw;
}

void Catalog::DropIndex(const std::string& name) {
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

bool Catalog::KeyTaken(IndexInfo* idx, const Value& v, const std::function<bool(RID)>& ignore) {
    TableInfo* t = idx->hashed ? RequireTable(idx->table) : nullptr;
    const int64_t key = idx->KeyOf(v);
    std::string buf;
    for (auto it = idx->tree->LowerBound(key); it.Valid() && it.Entry().key == key; it.Next()) {
        RID rid = it.Entry().rid;
        if (ignore && ignore(rid)) continue;
        if (!idx->hashed) return true;
        // Same hash: only a conflict if the actual text is equal too.
        if (!t->heap->Get(rid, &buf)) continue;
        Row row = DeserializeTuple(t->schema, buf.data(), buf.size());
        const Value& other = row[static_cast<size_t>(idx->col_idx)];
        if (!other.IsNull() && other.Compare(v) == 0) return true;
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

}  // namespace minidb
