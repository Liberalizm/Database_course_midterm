// Unit tests: TablePage (slotted page layout), operating on a plain in-memory buffer.
#include "table_page.h"
#include "test_util.h"

namespace minidb {
namespace {

class TablePageTest : public ::testing::Test {
protected:
    void SetUp() override { page.Init(); }
    std::optional<uint16_t> Put(const std::string& s) { return page.Insert(s.data(), s.size()); }
    std::string GetStr(uint16_t slot) {
        std::string out;
        EXPECT_TRUE(page.Get(slot, &out));
        return out;
    }

    alignas(8) char buf[PAGE_SIZE];
    TablePage page{buf};
};

TEST_F(TablePageTest, InitialState) {
    EXPECT_EQ(page.NumSlots(), 0);
    EXPECT_EQ(page.NextPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(page.FreeSpace(), PAGE_SIZE - TablePage::HEADER);
    EXPECT_EQ(page.LiveCount(), 0u);
}

TEST_F(TablePageTest, InsertAndGet) {
    auto a = Put("first"), b = Put("second");
    ASSERT_TRUE(a && b);
    EXPECT_EQ(*a, 0);
    EXPECT_EQ(*b, 1);
    EXPECT_EQ(GetStr(*a), "first");
    EXPECT_EQ(GetStr(*b), "second");
    std::string out;
    EXPECT_FALSE(page.Get(2, &out));
}

TEST_F(TablePageTest, FillsUntilFull) {
    std::string tuple(100, 'x');
    int n = 0;
    while (Put(tuple)) ++n;
    EXPECT_EQ(n, static_cast<int>((PAGE_SIZE - TablePage::HEADER) / (100 + TablePage::SLOT)));
    EXPECT_LT(page.FreeSpace(), 100 + TablePage::SLOT);
}

TEST_F(TablePageTest, RejectsEmptyAndOversizedTuples) {
    EXPECT_FALSE(page.Insert("", 0));
    std::string too_big(TablePage::MAX_TUPLE + 1, 'x');
    EXPECT_FALSE(Put(too_big));
    std::string max(TablePage::MAX_TUPLE, 'm');
    auto s = Put(max);
    ASSERT_TRUE(s);
    EXPECT_EQ(GetStr(*s), max);
    EXPECT_EQ(page.FreeSpace(), 0u);
}

TEST_F(TablePageTest, DeleteFreesSlotForReuse) {
    Put("a");
    auto b = Put("b");
    Put("c");
    EXPECT_TRUE(page.Delete(*b));
    EXPECT_FALSE(page.Delete(*b)) << "double delete";
    std::string out;
    EXPECT_FALSE(page.Get(*b, &out));
    auto d = Put("d");
    EXPECT_EQ(*d, *b) << "empty slot must be reused";
    EXPECT_EQ(page.NumSlots(), 3);
}

TEST_F(TablePageTest, DeletingTrailingSlotsShrinksSlotArray) {
    Put("a");
    auto b = Put("b");
    auto c = Put("c");
    page.Delete(*b);
    EXPECT_EQ(page.NumSlots(), 3);
    page.Delete(*c);
    EXPECT_EQ(page.NumSlots(), 1);
}

TEST_F(TablePageTest, CompactionReclaimsHolesAndKeepsSlotNumbers) {
    std::string tuple(100, 'x');
    std::vector<uint16_t> slots;
    while (auto s = Put(tuple)) slots.push_back(*s);
    for (size_t i = 0; i < slots.size(); i += 2) page.Delete(slots[i]);
    std::string big(1000, 'y');
    auto s = Put(big);  // only fits after compaction
    ASSERT_TRUE(s);
    EXPECT_EQ(GetStr(*s), big);
    for (size_t i = 1; i < slots.size(); i += 2) EXPECT_EQ(GetStr(slots[i]), tuple) << "slot " << slots[i];
}

TEST_F(TablePageTest, UpdateShrinkInPlace) {
    auto s = Put("a fairly long value");
    ASSERT_TRUE(page.Update(*s, "short", 5));
    EXPECT_EQ(GetStr(*s), "short");
}

TEST_F(TablePageTest, UpdateGrowUsesFreeSpaceOrCompaction) {
    auto a = Put(std::string(100, 'a'));
    std::string bigger(300, 'b');
    ASSERT_TRUE(page.Update(*a, bigger.data(), bigger.size()));
    EXPECT_EQ(GetStr(*a), bigger);

    // fill the page, then grow a tuple: needs compaction of the freed old version
    std::vector<uint16_t> others;
    while (auto s = Put(std::string(50, 'f'))) others.push_back(*s);
    page.Delete(others[0]);
    page.Delete(others[1]);
    std::string grown(380, 'g');
    ASSERT_TRUE(page.Update(*a, grown.data(), grown.size()));
    EXPECT_EQ(GetStr(*a), grown);
    for (size_t i = 2; i < others.size(); ++i) EXPECT_EQ(GetStr(others[i]), std::string(50, 'f'));
}

TEST_F(TablePageTest, UpdateThatCannotFitReturnsFalseAndKeepsOldValue) {
    std::vector<uint16_t> slots;
    while (auto s = Put(std::string(200, 'z'))) slots.push_back(*s);
    std::string huge(2000, 'h');
    EXPECT_FALSE(page.Update(slots[0], huge.data(), huge.size()));
    EXPECT_EQ(GetStr(slots[0]), std::string(200, 'z'));
}

TEST_F(TablePageTest, UpdateOfDeletedTupleThrows) {
    auto s = Put("x");
    page.Delete(*s);
    EXPECT_THROW(page.Update(*s, "y", 1), DbError);
}

TEST_F(TablePageTest, NextPagePointer) {
    page.SetNextPageId(17);
    EXPECT_EQ(page.NextPageId(), 17);
}

}  // namespace
}  // namespace minidb
