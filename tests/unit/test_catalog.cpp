// Unit tests: Catalog (table/index metadata and its persistence in page 0).
#include "catalog.h"
#include "test_util.h"

namespace minidb {
namespace {

using namespace testing;

class CatalogTest : public ::testing::Test {
protected:
    TempFile file;
    std::unique_ptr<DiskManager> disk = std::make_unique<DiskManager>(file.path());
    std::unique_ptr<BufferPoolManager> bpm = std::make_unique<BufferPoolManager>(32, disk.get());
    std::unique_ptr<Catalog> cat = std::make_unique<Catalog>(bpm.get());

    void Reopen() {
        cat->Save();
        cat.reset();
        bpm.reset();
        disk.reset();
        disk = std::make_unique<DiskManager>(file.path());
        bpm = std::make_unique<BufferPoolManager>(32, disk.get());
        cat = std::make_unique<Catalog>(bpm.get());
    }

    // Saves the catalog without its last `bytes` bytes and reopens it: this is how
    // a file written by an older version (before a trailing section existed) looks.
    void TruncateSavedCatalog(uint32_t bytes) {
        cat->Save();
        {
            PageGuard g = PageGuard::Fetch(bpm.get(), CATALOG_PAGE_ID);
            uint32_t total = ReadAs<uint32_t>(g.Data() + 4);
            WriteAs<uint32_t>(g.Data() + 4, total - bytes);
            g.MarkDirty();
        }
        bpm->FlushAll();
        cat.reset();
        bpm.reset();
        disk.reset();
        disk = std::make_unique<DiskManager>(file.path());
        bpm = std::make_unique<BufferPoolManager>(32, disk.get());
        cat = std::make_unique<Catalog>(bpm.get());
    }

    static void AddRow(TableInfo* t, const Row& r) {
        t->heap->Insert(SerializeTuple(t->schema, r));
        ++t->row_count;
    }
};

TEST_F(CatalogTest, FreshDatabaseUsesPageZero) {
    EXPECT_EQ(disk->NumPages(), 1);
    EXPECT_TRUE(cat->Tables().empty());
}

TEST_F(CatalogTest, CreateAndLookupTable) {
    TableInfo* t = cat->CreateTable("users", {Col("id", TypeId::INT), Col("name", TypeId::TEXT)});
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(cat->GetTable("users"), t);
    EXPECT_EQ(t->schema.Size(), 2u);
    EXPECT_EQ(t->schema.At(1).table, "users");
    EXPECT_EQ(cat->GetTable("nope"), nullptr);
    EXPECT_THROW(cat->RequireTable("nope"), DbError);
}

TEST_F(CatalogTest, RejectsInvalidTables) {
    cat->CreateTable("t", {Col("a", TypeId::INT)});
    EXPECT_THROW(cat->CreateTable("t", {Col("a", TypeId::INT)}), DbError);
    EXPECT_THROW(cat->CreateTable("u", {Col("a", TypeId::INT), Col("a", TypeId::TEXT)}), DbError);
    EXPECT_THROW(cat->CreateTable("v", {}), DbError);
}

TEST_F(CatalogTest, IndexIsBuiltFromExistingRows) {
    TableInfo* t = cat->CreateTable("t", {Col("k", TypeId::INT), Col("s", TypeId::TEXT)});
    for (int i = 0; i < 300; ++i) AddRow(t, {i % 10 == 0 ? N() : I(i), T("x")});
    IndexInfo* idx = cat->CreateIndex("t_k", "t", "k", false);
    EXPECT_EQ(idx->col_idx, 0);
    EXPECT_EQ(idx->tree->CheckAndCount(), 270u) << "NULL keys are not indexed";
    ASSERT_EQ(cat->IndexesOf("t").size(), 1u);
}

TEST_F(CatalogTest, IndexValidation) {
    TableInfo* t = cat->CreateTable("t", {Col("k", TypeId::INT), Col("s", TypeId::TEXT), Col("d", TypeId::DOUBLE)});
    EXPECT_THROW(cat->CreateIndex("i1", "t", "d", false), DbError) << "DOUBLE not indexable";
    EXPECT_THROW(cat->CreateIndex("i2", "t", "zzz", false), DbError);
    EXPECT_THROW(cat->CreateIndex("i3", "missing", "k", false), DbError);
    AddRow(t, {I(1), T("a"), D(1.0)});
    AddRow(t, {I(1), T("b"), D(2.0)});
    EXPECT_THROW(cat->CreateIndex("uniq", "t", "k", true), DbError) << "duplicates violate UNIQUE";
    EXPECT_NO_THROW(cat->CreateIndex("i4", "t", "k", false));
    EXPECT_THROW(cat->CreateIndex("i4", "t", "k", false), DbError) << "duplicate index name";
}

TEST_F(CatalogTest, DropTableAlsoDropsItsIndexes) {
    cat->CreateTable("a", {Col("k", TypeId::INT)});
    cat->CreateTable("b", {Col("k", TypeId::INT)});
    cat->CreateIndex("a_k", "a", "k", false);
    cat->CreateIndex("b_k", "b", "k", false);
    cat->DropTable("a");
    EXPECT_EQ(cat->GetTable("a"), nullptr);
    ASSERT_EQ(cat->Indexes().size(), 1u);
    EXPECT_EQ(cat->Indexes()[0]->name, "b_k");
    EXPECT_THROW(cat->DropTable("a"), DbError);
    EXPECT_THROW(cat->DropIndex("a_k"), DbError);
    cat->DropIndex("b_k");
    EXPECT_TRUE(cat->Indexes().empty());
}

TEST_F(CatalogTest, EverythingSurvivesReopen) {
    std::vector<Column> cols = {Col("id", TypeId::INT), Col("score", TypeId::DOUBLE), Col("ok", TypeId::BOOL)};
    cols[0].not_null = true;
    TableInfo* t = cat->CreateTable("scores", cols);
    for (int i = 0; i < 500; ++i) AddRow(t, {I(i), D(i * 0.5), B(i % 2 == 0)});
    cat->CreateIndex("scores_pkey", "scores", "id", true);

    Reopen();

    TableInfo* r = cat->RequireTable("scores");
    EXPECT_EQ(r->row_count, 500);
    ASSERT_EQ(r->schema.Size(), 3u);
    EXPECT_EQ(r->schema.At(1).type, TypeId::DOUBLE);
    EXPECT_TRUE(r->schema.At(0).not_null);
    EXPECT_FALSE(r->schema.At(1).not_null);
    auto idx = cat->IndexesOf("scores");
    ASSERT_EQ(idx.size(), 1u);
    EXPECT_TRUE(idx[0]->unique);
    EXPECT_EQ(idx[0]->tree->CheckAndCount(), 500u);
    EXPECT_TRUE(idx[0]->tree->ContainsKey(499));
}

TEST_F(CatalogTest, LargeCatalogSpansMultiplePages) {
    for (int i = 0; i < 150; ++i) {
        std::vector<Column> cols;
        for (int c = 0; c < 4; ++c) cols.push_back(Col("a_rather_long_column_name_" + std::to_string(c), TypeId::INT));
        cat->CreateTable("table_number_" + std::to_string(i), cols);
    }
    Reopen();
    EXPECT_EQ(cat->Tables().size(), 150u);
    EXPECT_NE(cat->GetTable("table_number_149"), nullptr);
}

TEST_F(CatalogTest, RejectsForeignFile) {
    cat.reset();
    bpm.reset();
    disk.reset();
    {
        DiskManager d(file.path());
        char junk[PAGE_SIZE];
        std::memset(junk, 0, PAGE_SIZE);
        WriteAs<uint32_t>(junk + 4, 12);
        std::memcpy(junk + 8, "\x08\0\0\0NOTADBXX", 12);
        d.WritePage(0, junk);
    }
    disk = std::make_unique<DiskManager>(file.path());
    bpm = std::make_unique<BufferPoolManager>(32, disk.get());
    EXPECT_THROW(Catalog c(bpm.get()), DbError);
}

// Fills a table with enough rows to span many heap and index pages.
static void FillWithIndex(Catalog* cat, const std::string& name, int rows) {
    TableInfo* t = cat->CreateTable(name, {Col("id", TypeId::INT), Col("pad", TypeId::TEXT)});
    for (int i = 0; i < rows; ++i) {
        t->heap->Insert(SerializeTuple(t->schema, {Value::Int(i), Value::Text(std::string(100, 'p'))}));
        ++t->row_count;
    }
    cat->CreateIndex(name + "_id", name, "id", true);
}

TEST_F(CatalogTest, DropTableFreesHeapAndIndexPagesForReuse) {
    FillWithIndex(cat.get(), "a", 2000);
    page_id_t size_with_a = disk->NumPages();
    cat->DropTable("a");
    EXPECT_GT(bpm->FreePages().size(), 50u) << "heap + index pages are given back";

    FillWithIndex(cat.get(), "b", 2000);
    EXPECT_EQ(disk->NumPages(), size_with_a) << "the same amount of data fits in the freed pages";
    TableInfo* b = cat->RequireTable("b");
    EXPECT_EQ(cat->IndexesOf("b")[0]->tree->CheckAndCount(), 2000u);
    size_t rows = 0;
    auto it = b->heap->Begin();
    RID rid;
    std::string buf;
    while (it.Next(&rid, &buf)) ++rows;
    EXPECT_EQ(rows, 2000u) << "reused pages start out empty";
}

TEST_F(CatalogTest, DropIndexFreesItsPages) {
    FillWithIndex(cat.get(), "a", 2000);
    size_t tree_pages = cat->IndexesOf("a")[0]->tree->Pages().size();
    EXPECT_GT(tree_pages, 1u);
    cat->DropIndex("a_id");
    EXPECT_EQ(bpm->FreePages().size(), tree_pages);
}

TEST_F(CatalogTest, FreePageListSurvivesRestart) {
    FillWithIndex(cat.get(), "a", 2000);
    cat->CreateTable("keep", {Col("x", TypeId::INT)});
    cat->DropTable("a");
    auto free_before = bpm->FreePages();
    page_id_t size = disk->NumPages();
    Reopen();
    EXPECT_EQ(bpm->FreePages(), free_before);
    FillWithIndex(cat.get(), "c", 2000);
    EXPECT_EQ(disk->NumPages(), size) << "pages freed before the restart are reused after it";
    EXPECT_NE(cat->GetTable("keep"), nullptr);
}

TEST_F(CatalogTest, FailedUniqueIndexBuildDoesNotLeakPages) {
    TableInfo* t = cat->CreateTable("t", {Col("k", TypeId::INT)});
    for (int i = 0; i < 2000; ++i) AddRow(t, {Value::Int(i)});
    AddRow(t, {Value::Int(5)});  // duplicate found only after the tree has grown
    EXPECT_THROW(cat->CreateIndex("t_k", "t", "k", true), DbError);
    EXPECT_GT(bpm->FreePages().size(), 1u);
    EXPECT_TRUE(cat->IndexesOf("t").empty());
}

TEST_F(CatalogTest, CatalogGrowthDoesNotHandOutItsOwnPages) {
    // Free some pages, then make the catalog big enough to need extra pages:
    // the extension takes pages from the free list, and the list saved to disk
    // must not still contain them.
    // (The tables are created first: each one takes a heap page, which would
    // otherwise use up the free list before the catalog grows.)
    for (int i = 0; i < 150; ++i) {
        std::vector<Column> cols;
        for (int c = 0; c < 4; ++c) cols.push_back(Col("a_rather_long_column_name_" + std::to_string(c), TypeId::INT));
        cat->CreateTable("table_number_" + std::to_string(i), cols);
    }
    FillWithIndex(cat.get(), "a", 2000);
    cat->DropTable("a");
    ASSERT_GT(bpm->FreePages().size(), 10u);
    Reopen();  // saving grows the catalog chain using free pages
    EXPECT_EQ(cat->Tables().size(), 150u);
    // Walk the catalog's own page chain (next pointer in the first 4 bytes).
    int chain = 0;
    for (page_id_t p = CATALOG_PAGE_ID; p != INVALID_PAGE_ID; ++chain) {
        EXPECT_EQ(bpm->FreePages().count(p), 0u) << "catalog page " << p << " is on the free list";
        PageGuard g = PageGuard::Fetch(bpm.get(), p);
        p = ReadAs<page_id_t>(g.Data());
    }
    EXPECT_GT(chain, 1) << "the catalog must have grown for this test to mean anything";

    FillWithIndex(cat.get(), "big", 3000);  // must not land on catalog pages
    Reopen();
    EXPECT_EQ(cat->Tables().size(), 151u);
    EXPECT_EQ(cat->IndexesOf("big")[0]->tree->CheckAndCount(), 3000u);
    size_t rows = 0;
    auto it = cat->RequireTable("big")->heap->Begin();
    RID rid;
    std::string buf;
    while (it.Next(&rid, &buf)) ++rows;
    EXPECT_EQ(rows, 3000u) << "heap pages were overwritten by the catalog";
}

// A crash right after creating a database (before any Save/flush) used to leave
// an all-zero page 0 behind, and the file could never be opened again.
TEST_F(CatalogTest, NewDatabaseIsDurableBeforeAnyStatement) {
    std::string copy = file.path() + ".crash";
    std::filesystem::copy_file(file.path(), copy, std::filesystem::copy_options::overwrite_existing);
    {
        DiskManager d(copy);
        BufferPoolManager b(16, &d);
        EXPECT_NO_THROW(Catalog c(&b));
    }
    std::remove(copy.c_str());
}

TEST_F(CatalogTest, AllZeroSinglePageFileOpensAsEmptyDatabase) {
    cat.reset();
    bpm.reset();
    disk.reset();
    {
        DiskManager d(file.path());
        char zeros[PAGE_SIZE] = {};
        d.WritePage(0, zeros);
    }
    disk = std::make_unique<DiskManager>(file.path());
    bpm = std::make_unique<BufferPoolManager>(32, disk.get());
    cat = std::make_unique<Catalog>(bpm.get());
    EXPECT_TRUE(cat->Tables().empty());
    cat->CreateTable("t", {Col("a", TypeId::INT)});
    Reopen();
    EXPECT_NE(cat->GetTable("t"), nullptr) << "the repaired file works normally";
}

TEST_F(CatalogTest, ZeroedCatalogWithOtherPagesIsStillCorrupt) {
    cat->CreateTable("t", {Col("a", TypeId::INT)});
    cat->Save();
    bpm->FlushAll();
    cat.reset();
    bpm.reset();
    disk.reset();
    {
        DiskManager d(file.path());
        ASSERT_GT(d.NumPages(), 1);
        char zeros[PAGE_SIZE] = {};
        d.WritePage(0, zeros);
    }
    disk = std::make_unique<DiskManager>(file.path());
    bpm = std::make_unique<BufferPoolManager>(32, disk.get());
    EXPECT_THROW(Catalog c(bpm.get()), DbError) << "data exists: never silently reset it";
}

// ---- TEXT (hashed) indexes ----

TEST_F(CatalogTest, TextIndexIsHashedAndExact) {
    EXPECT_EQ(HashTextKey("abc"), HashTextKey("abc"));
    EXPECT_NE(HashTextKey("abc"), HashTextKey("abd"));
    EXPECT_NE(HashTextKey(""), HashTextKey(std::string(1, '\0')));

    TableInfo* t = cat->CreateTable("u", {Col("id", TypeId::INT), Col("email", TypeId::TEXT)});
    AddRow(t, {I(1), T("a@x")});
    AddRow(t, {I(2), T("b@x")});
    AddRow(t, {I(3), N()});
    IndexInfo* idx = cat->CreateIndex("u_email", "u", "email", true);
    EXPECT_TRUE(idx->hashed);
    EXPECT_EQ(idx->tree->CheckAndCount(), 2u) << "NULLs are not indexed";
    EXPECT_TRUE(cat->KeyTaken(idx, T("a@x")));
    EXPECT_FALSE(cat->KeyTaken(idx, T("c@x")));
    EXPECT_FALSE(cat->KeyTaken(idx, T("a@x"), [](RID) { return true; })) << "ignored rows do not count";
}

TEST_F(CatalogTest, TextIndexSurvivesHashCollisions) {
    TableInfo* t = cat->CreateTable("u", {Col("id", TypeId::INT), Col("name", TypeId::TEXT)});
    AddRow(t, {I(1), T("y")});
    IndexInfo* idx = cat->CreateIndex("u_name", "u", "name", true);
    // Fake a collision: an entry with the hash of "x" that points at the row holding "y".
    RID y_rid;
    {
        auto it = t->heap->Begin();
        std::string buf;
        ASSERT_TRUE(it.Next(&y_rid, &buf));
    }
    idx->tree->Insert(HashTextKey("x"), y_rid);
    EXPECT_FALSE(cat->KeyTaken(idx, T("x"))) << "same hash, different text: not a duplicate";
    EXPECT_TRUE(cat->KeyTaken(idx, T("y")));
}

TEST_F(CatalogTest, TextIndexKindIsRestoredAfterRestart) {
    TableInfo* t = cat->CreateTable("u", {Col("id", TypeId::INT), Col("name", TypeId::TEXT)});
    AddRow(t, {I(1), T("ann")});
    cat->CreateIndex("u_name", "u", "name", true);
    cat->CreateIndex("u_id", "u", "id", false);
    Reopen();
    for (IndexInfo* idx : cat->IndexesOf("u")) EXPECT_EQ(idx->hashed, idx->column == "name") << idx->name;
    IndexInfo* by_name = cat->IndexesOf("u")[0]->column == "name" ? cat->IndexesOf("u")[0] : cat->IndexesOf("u")[1];
    EXPECT_TRUE(cat->KeyTaken(by_name, T("ann")));
}

TEST_F(CatalogTest, UniqueTextIndexOnDuplicatesFails) {
    TableInfo* t = cat->CreateTable("u", {Col("name", TypeId::TEXT)});
    AddRow(t, {T("a")});
    AddRow(t, {T("b")});
    AddRow(t, {T("a")});
    EXPECT_THROW(cat->CreateIndex("u_name", "u", "name", true), DbError);
    EXPECT_NO_THROW(cat->CreateIndex("u_name", "u", "name", false));
}

// ---- ALTER TABLE support ----

static size_t ScanCount(TableInfo* t) {
    size_t n = 0;
    auto it = t->heap->Begin();
    RID rid;
    std::string buf;
    while (it.Next(&rid, &buf)) ++n;
    return n;
}

TEST_F(CatalogTest, RenameTableAndColumnCarryIndexesAlong) {
    TableInfo* t = cat->CreateTable("t", {Col("id", TypeId::INT), Col("v", TypeId::INT)});
    AddRow(t, {I(1), I(10)});
    cat->CreateIndex("t_v", "t", "v", false);
    cat->RenameColumn("t", "v", "w");
    EXPECT_EQ(cat->IndexesOf("t")[0]->column, "w");
    EXPECT_THROW(cat->RenameColumn("t", "w", "id"), DbError) << "name taken";
    EXPECT_THROW(cat->RenameColumn("t", "nope", "x"), DbError);
    cat->CreateTable("other", {Col("a", TypeId::INT)});
    EXPECT_THROW(cat->RenameTable("t", "other"), DbError);
    cat->RenameTable("t", "u");
    EXPECT_EQ(cat->GetTable("t"), nullptr);
    TableInfo* u = cat->RequireTable("u");
    EXPECT_EQ(u->name, "u");
    EXPECT_EQ(u->schema.At(1).table, "u");
    ASSERT_EQ(cat->IndexesOf("u").size(), 1u);
    Reopen();
    EXPECT_EQ(cat->RequireTable("u")->schema.At(1).name, "w");
    ASSERT_EQ(cat->IndexesOf("u").size(), 1u);
    EXPECT_EQ(cat->IndexesOf("u")[0]->col_idx, 1);
}

TEST_F(CatalogTest, RewriteTableConvertsRowsRebuildsIndexesAndFreesPages) {
    TableInfo* t = cat->CreateTable("t", {Col("id", TypeId::INT), Col("drop_me", TypeId::INT), Col("v", TypeId::INT)});
    for (int i = 0; i < 2000; ++i) AddRow(t, {I(i), I(i % 7), I(i * 2)});
    cat->CreateIndex("t_drop", "t", "drop_me", false);
    cat->CreateIndex("t_v", "t", "v", true);
    page_id_t size = disk->NumPages();
    // Drop the middle column.
    cat->RewriteTable(t, {Col("id", TypeId::INT), Col("v", TypeId::INT)}, [](Row r) {
        r.erase(r.begin() + 1);
        return r;
    });
    EXPECT_EQ(t->schema.Size(), 2u);
    EXPECT_EQ(ScanCount(t), 2000u);
    ASSERT_EQ(cat->IndexesOf("t").size(), 1u) << "the index on the dropped column is gone";
    IndexInfo* v = cat->IndexesOf("t")[0];
    EXPECT_EQ(v->col_idx, 1) << "rebuilt against the new column positions";
    EXPECT_EQ(v->tree->CheckAndCount(), 2000u);
    EXPECT_TRUE(cat->KeyTaken(v, I(3998)));
    EXPECT_FALSE(bpm->FreePages().empty()) << "old heap pages were freed";
    EXPECT_LE(disk->NumPages(), size + 2) << "the rewrite reuses the dropped index's pages";
}

TEST_F(CatalogTest, FailedRewriteLeavesTheTableUntouched) {
    TableInfo* t = cat->CreateTable("t", {Col("id", TypeId::INT), Col("s", TypeId::TEXT)});
    AddRow(t, {I(1), T("short")});
    AddRow(t, {I(2), T(std::string(TablePage::MAX_TUPLE - 40, 'x'))});
    cat->CreateIndex("t_id", "t", "id", true);
    page_id_t first = t->first_page;
    auto grow = [](Row r) {  // makes the second row too large
        r.push_back(Value::Text(std::string(100, 'y')));
        return r;
    };
    EXPECT_THROW(cat->RewriteTable(t, {Col("id", TypeId::INT), Col("s", TypeId::TEXT), Col("z", TypeId::TEXT)}, grow),
                 DbError);
    EXPECT_EQ(t->schema.Size(), 2u);
    EXPECT_EQ(t->first_page, first);
    EXPECT_EQ(ScanCount(t), 2u);
    EXPECT_EQ(cat->IndexesOf("t").size(), 1u) << "indexes are only dropped after all checks passed";
}

TEST_F(CatalogTest, ColumnDefaultsOfEveryTypeSurviveRestart) {
    std::vector<Column> cols = {Col("i", TypeId::INT), Col("d", TypeId::DOUBLE), Col("b", TypeId::BOOL),
                                Col("t", TypeId::TEXT), Col("n", TypeId::INT), Col("none", TypeId::INT)};
    cols[0].default_value = I(-42);
    cols[1].default_value = D(2.75);
    cols[2].default_value = B(true);
    cols[3].default_value = T("hello");
    cols[4].default_value = N();  // an explicit DEFAULT NULL
    cat->CreateTable("t", cols);
    Reopen();
    const Schema& s = cat->RequireTable("t")->schema;
    ASSERT_TRUE(s.At(0).default_value && s.At(1).default_value && s.At(2).default_value && s.At(3).default_value);
    EXPECT_EQ(s.At(0).default_value->AsInt(), -42);
    EXPECT_DOUBLE_EQ(s.At(1).default_value->AsDouble(), 2.75);
    EXPECT_TRUE(s.At(2).default_value->AsBool());
    EXPECT_EQ(s.At(3).default_value->AsText(), "hello");
    ASSERT_TRUE(s.At(4).default_value);
    EXPECT_TRUE(s.At(4).default_value->IsNull());
    EXPECT_FALSE(s.At(5).default_value);
}

TEST_F(CatalogTest, CatalogWithoutDefaultsSectionStillLoads) {
    // A file written before DEFAULT existed ends right after the free-page list.
    // Simulate it by cutting the (empty) defaults and views sections off the saved catalog.
    cat->CreateTable("t", {Col("a", TypeId::INT)});
    TruncateSavedCatalog(8);  // the trailing "0 defaults" and "0 views" counts
    EXPECT_NE(cat->GetTable("t"), nullptr);
    EXPECT_FALSE(cat->RequireTable("t")->schema.At(0).default_value);
}

TEST_F(CatalogTest, ViewsSurviveRestart) {
    cat->CreateTable("t", {Col("a", TypeId::INT)});
    cat->CreateView("v", {}, "SELECT a FROM t WHERE a > 1", false);
    cat->CreateView("w", {"x", "y"}, "SELECT a, a * 2\nFROM t -- comment", false);
    Reopen();
    ASSERT_EQ(cat->Views().size(), 2u);
    ViewInfo* v = cat->GetView("v");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->sql, "SELECT a FROM t WHERE a > 1");
    EXPECT_TRUE(v->columns.empty());
    ViewInfo* w = cat->GetView("w");
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->sql, "SELECT a, a * 2\nFROM t -- comment");
    EXPECT_EQ(w->columns, (std::vector<std::string>{"x", "y"}));
    cat->DropView("v");
    Reopen();
    EXPECT_EQ(cat->GetView("v"), nullptr);
    EXPECT_NE(cat->GetView("w"), nullptr);
}

TEST_F(CatalogTest, TablesAndViewsShareOneNamespace) {
    cat->CreateTable("t", {Col("a", TypeId::INT)});
    cat->CreateTable("u", {Col("a", TypeId::INT)});
    cat->CreateView("v", {}, "SELECT 1", false);
    EXPECT_THROW(cat->CreateView("t", {}, "SELECT 1", false), DbError);
    EXPECT_THROW(cat->CreateView("t", {}, "SELECT 1", true), DbError) << "OR REPLACE never replaces a table";
    EXPECT_THROW(cat->CreateView("v", {}, "SELECT 2", false), DbError);
    EXPECT_THROW(cat->CreateTable("v", {Col("a", TypeId::INT)}), DbError);
    EXPECT_THROW(cat->RenameTable("u", "v"), DbError);
    EXPECT_THROW(cat->CreateView("w", {"a", "a"}, "SELECT 1, 2", false), DbError) << "duplicate column names";

    cat->CreateView("v", {}, "SELECT 2", true);
    EXPECT_EQ(cat->GetView("v")->sql, "SELECT 2");
    EXPECT_EQ(cat->Views().size(), 1u);

    // A view is not a table: everything that needs one says so.
    EXPECT_EQ(cat->GetTable("v"), nullptr);
    try {
        cat->RequireTable("v");
        FAIL() << "expected an error";
    } catch (const DbError& e) {
        EXPECT_NE(std::string(e.what()).find("is a view"), std::string::npos) << e.what();
    }
    EXPECT_THROW(cat->DropTable("v"), DbError);
    EXPECT_THROW(cat->CreateIndex("v_i", "v", "a", false), DbError);
    EXPECT_THROW(cat->DropView("t"), DbError);
    EXPECT_THROW(cat->DropView("nope"), DbError);
}

TEST_F(CatalogTest, CatalogWithoutViewsSectionStillLoads) {
    cat->CreateTable("t", {Col("a", TypeId::INT)});
    TruncateSavedCatalog(4);  // drop the trailing "0 views" count
    EXPECT_NE(cat->GetTable("t"), nullptr);
    EXPECT_TRUE(cat->Views().empty());
}

}  // namespace
}  // namespace minidb
