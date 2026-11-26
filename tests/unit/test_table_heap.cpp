// Unit tests: TableHeap (a table as a linked list of slotted pages).
#include <map>
#include <random>

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

// Differential test: random inserts, deletes and size-changing updates (which
// move tuples between pages) against a std::map, with a tiny buffer pool.
TEST_F(TableHeapTest, RandomOperationsMatchReferenceModel) {
    std::mt19937 rng(7);
    std::map<RID, std::string> ref;
    auto random_tuple = [&] {
        size_t len = rng() % 8 == 0 ? 500 + rng() % 1500 : 1 + rng() % 60;  // mostly small, some big
        std::string s(len, static_cast<char>('a' + rng() % 26));
        s[0] = static_cast<char>('A' + rng() % 26);
        return s;
    };
    for (int step = 0; step < 6000; ++step) {
        int op = static_cast<int>(rng() % 10);
        if (ref.empty() || op < 4) {
            std::string tup = random_tuple();
            RID rid = heap.Insert(tup);
            ASSERT_EQ(ref.count(rid), 0u) << "Insert returned a live RID at step " << step;
            ref[rid] = tup;
        } else {
            auto it = ref.begin();
            std::advance(it, static_cast<long>(rng() % ref.size()));
            RID rid = it->first;
            if (op < 7) {
                ASSERT_TRUE(heap.Delete(rid));
                ASSERT_FALSE(heap.Delete(rid)) << "double delete must fail";
                ref.erase(it);
            } else {
                std::string tup = random_tuple();
                RID nrid = heap.Update(rid, tup);
                ref.erase(it);
                ASSERT_EQ(ref.count(nrid), 0u) << "Update moved onto a live RID at step " << step;
                ref[nrid] = tup;
            }
        }
        if (step % 250 == 0) {
            ASSERT_EQ(ScanAll(), ref) << "step " << step;
            std::string got;
            for (const auto& [rid, tup] : ref) {
                ASSERT_TRUE(heap.Get(rid, &got));
                ASSERT_EQ(got, tup);
            }
        }
    }
    ASSERT_EQ(ScanAll(), ref);
    EXPECT_EQ(bpm.PinnedFrames(), 0u);
}

}  // namespace
}  // namespace minidb
