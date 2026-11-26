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
    TableInfo* t = cat->CreateTable("t", {Col("k", TypeId::INT), Col("s", TypeId::TEXT)});
    EXPECT_THROW(cat->CreateIndex("i1", "t", "s", false), DbError) << "TEXT not indexable";
    EXPECT_THROW(cat->CreateIndex("i2", "t", "zzz", false), DbError);
    EXPECT_THROW(cat->CreateIndex("i3", "missing", "k", false), DbError);
    AddRow(t, {I(1), T("a")});
    AddRow(t, {I(1), T("b")});
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

}  // namespace
}  // namespace minidb
