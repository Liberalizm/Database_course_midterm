// Unit tests: TableHeap (a table as a linked list of slotted pages).
#include <map>

#include "table_heap.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

class TableHeapTest : public ::testing::Test {
protected:
    TempFile file;
    DiskManager disk{file.path()};
    BufferPoolManager bpm{16, &disk};
    page_id_t first = TableHeap::Create(&bpm);
    TableHeap heap{&bpm, first};

    std::map<RID, std::string> ScanAll() {
        std::map<RID, std::string> out;
        auto it = heap.Begin();
        RID rid;
        std::string data;
        while (it.Next(&rid, &data)) out[rid] = data;
        return out;
    }
};

TEST_F(TableHeapTest, EmptyHeapHasOnePageAndNoTuples) {
    EXPECT_EQ(heap.PageCount(), 1u);
    EXPECT_TRUE(ScanAll().empty());
}

TEST_F(TableHeapTest, InsertGetAcrossManyPages) {
    std::vector<RID> rids;
    for (int i = 0; i < 2000; ++i) rids.push_back(heap.Insert("tuple-" + std::to_string(i)));
    EXPECT_GT(heap.PageCount(), 5u);
    std::string out;
    for (int i = 0; i < 2000; ++i) {
        ASSERT_TRUE(heap.Get(rids[i], &out));
        EXPECT_EQ(out, "tuple-" + std::to_string(i));
    }
    EXPECT_EQ(ScanAll().size(), 2000u);
}

TEST_F(TableHeapTest, ScanSkipsDeletedTuples) {
    std::vector<RID> rids;
    for (int i = 0; i < 500; ++i) rids.push_back(heap.Insert(std::to_string(i)));
    for (int i = 0; i < 500; i += 2) EXPECT_TRUE(heap.Delete(rids[i]));
    EXPECT_FALSE(heap.Delete(rids[0]));
    auto all = ScanAll();
    EXPECT_EQ(all.size(), 250u);
    for (auto& [rid, data] : all) EXPECT_EQ(std::stoi(data) % 2, 1);
    std::string out;
    EXPECT_FALSE(heap.Get(rids[0], &out));
}

TEST_F(TableHeapTest, ReusesSpaceFreedByDeletes) {
    std::vector<RID> rids;
    for (int i = 0; i < 1000; ++i) rids.push_back(heap.Insert(std::string(50, 'a')));
    size_t pages = heap.PageCount();
    for (auto& r : rids) heap.Delete(r);
    for (int i = 0; i < 1000; ++i) heap.Insert(std::string(50, 'b'));
    EXPECT_EQ(heap.PageCount(), pages) << "deleted space should be reused, not new pages";
}

TEST_F(TableHeapTest, UpdateInPlaceKeepsRid) {
    RID r = heap.Insert("old value");
    RID r2 = heap.Update(r, "new");
    EXPECT_EQ(r, r2);
    std::string out;
    heap.Get(r2, &out);
    EXPECT_EQ(out, "new");
}

TEST_F(TableHeapTest, UpdateThatOverflowsPageMovesTuple) {
    std::vector<RID> rids;
    for (int i = 0; i < 30; ++i) rids.push_back(heap.Insert(std::string(130, 'x')));  // ~1 full page
    RID moved = heap.Update(rids[0], std::string(3000, 'y'));
    EXPECT_NE(moved, rids[0]);
    std::string out;
    EXPECT_FALSE(heap.Get(rids[0], &out));
    ASSERT_TRUE(heap.Get(moved, &out));
    EXPECT_EQ(out, std::string(3000, 'y'));
    EXPECT_EQ(ScanAll().size(), 30u);
}

TEST_F(TableHeapTest, OversizedTupleThrows) {
    EXPECT_THROW(heap.Insert(std::string(PAGE_SIZE, 'x')), DbError);
}

TEST_F(TableHeapTest, ReopenFindsAllPages) {
    for (int i = 0; i < 1000; ++i) heap.Insert("row " + std::to_string(i));
    TableHeap reopened(&bpm, first);
    EXPECT_EQ(reopened.PageCount(), heap.PageCount());
    RID r = reopened.Insert("after reopen");  // goes to the true last page
    EXPECT_EQ(r.page_id, ScanAll().rbegin()->first.page_id);
}

TEST_F(TableHeapTest, WorksWithTinyBufferPool) {
    // 16 frames, ~100 pages of data: correct results despite constant eviction.
    for (int i = 0; i < 5000; ++i) heap.Insert("v" + std::to_string(i) + std::string(60, '.'));
    EXPECT_GT(bpm.Evictions(), 50u);
    EXPECT_EQ(ScanAll().size(), 5000u);
    EXPECT_EQ(bpm.PinnedFrames(), 0u) << "no page may stay pinned";
}

}  // namespace
}  // namespace minidb
