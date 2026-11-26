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

}  // namespace
}  // namespace minidb
