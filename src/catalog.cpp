#include "catalog.h"

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
    } else {
        Load();
    }
}

void Catalog::Save() {
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

    const std::string& data = w.buf;
    size_t off = 0;
    page_id_t pid = CATALOG_PAGE_ID;
    while (true) {
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        char* d = g.Data();
        size_t chunk = std::min(CAT_CAP, data.size() - off);
        if (pid == CATALOG_PAGE_ID) WriteAs<uint32_t>(d + 4, static_cast<uint32_t>(data.size()));
        std::memcpy(d + CAT_HDR, data.data() + off, chunk);
        off += chunk;
        g.MarkDirty();
        if (off >= data.size()) break;
        page_id_t next = ReadAs<page_id_t>(d);
        if (next == INVALID_PAGE_ID) {  // grow the catalog chain
            PageGuard ng = PageGuard::New(bpm_, &next);
            WriteAs<page_id_t>(ng.Data(), INVALID_PAGE_ID);
            ng.MarkDirty();
            WriteAs<page_id_t>(d, next);
        }
        pid = next;
    }
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
        idx->tree = std::make_unique<BPlusTree>(bpm_, root);
        indexes_[idx->name] = std::move(idx);
    }
}

TableInfo* Catalog::CreateTable(const std::string& name, std::vector<Column> cols) {
    if (tables_.count(name)) throw DbError("table \"" + name + "\" already exists");
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
    if (!t) throw DbError("table \"" + name + "\" does not exist");
    return t;
}

void Catalog::DropTable(const std::string& name) {
    RequireTable(name);
    for (auto it = indexes_.begin(); it != indexes_.end();) {
        if (it->second->table == name)
            it = indexes_.erase(it);
        else
            ++it;
    }
    tables_.erase(name);
    // NOTE: the table's pages are not returned to a free list (no page reuse yet).
}

IndexInfo* Catalog::CreateIndex(const std::string& name, const std::string& table, const std::string& column,
                                bool unique) {
    if (indexes_.count(name)) throw DbError("index \"" + name + "\" already exists");
    TableInfo* t = RequireTable(table);
    int col = t->schema.Find("", column);
    if (col < 0) throw DbError("column \"" + column + "\" does not exist in table \"" + table + "\"");
    if (t->schema.At(col).type != TypeId::INT)
        throw DbError("indexes are only supported on INT columns (\"" + column + "\" is " +
                      TypeName(t->schema.At(col).type) + ")");

    auto idx = std::make_unique<IndexInfo>();
    idx->name = name;
    idx->table = table;
    idx->column = column;
    idx->col_idx = col;
    idx->unique = unique;
    idx->tree = std::make_unique<BPlusTree>(bpm_, INVALID_PAGE_ID);

    // Bulk-build from the existing rows.
    auto it = t->heap->Begin();
    RID rid;
    std::string buf;
    while (it.Next(&rid, &buf)) {
        Row row = DeserializeTuple(t->schema, buf.data(), buf.size());
        if (row[col].IsNull()) continue;
        int64_t key = row[col].AsInt();
        if (unique && idx->tree->ContainsKey(key))
            throw DbError("could not create unique index \"" + name + "\": duplicate key " + std::to_string(key));
        idx->tree->Insert(key, rid);
    }
    IndexInfo* raw = idx.get();
    indexes_[name] = std::move(idx);
    return raw;
}

void Catalog::DropIndex(const std::string& name) {
    if (!indexes_.erase(name)) throw DbError("index \"" + name + "\" does not exist");
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
