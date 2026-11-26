// Unit tests: BPlusTree (disk-based index).
#include <algorithm>
#include <climits>
#include <random>

#include "bplus_tree.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

class BPlusTreeTest : public ::testing::Test {
protected:
    TempFile file;
    DiskManager disk{file.path()};
    BufferPoolManager bpm{64, &disk};

    static std::vector<int64_t> Keys(const BPlusTree& t) {
        std::vector<int64_t> out;
        for (auto it = t.Begin(); it.Valid(); it.Next()) out.push_back(it.Entry().key);
        return out;
    }
};

TEST_F(BPlusTreeTest, EmptyTree) {
    BPlusTree t(&bpm, INVALID_PAGE_ID);
    EXPECT_FALSE(t.Begin().Valid());
    EXPECT_FALSE(t.LowerBound(5).Valid());
    EXPECT_EQ(t.Height(), 0);
    int64_t k;
    EXPECT_FALSE(t.MinKey(&k));
    EXPECT_FALSE(t.MaxKey(&k));
    EXPECT_FALSE(t.Remove(1, RID{1, 1}));
    EXPECT_FALSE(t.ContainsKey(1));
}

TEST_F(BPlusTreeTest, SingleEntry) {
    BPlusTree t(&bpm, INVALID_PAGE_ID);
    EXPECT_TRUE(t.Insert(10, RID{3, 4}));
    EXPECT_EQ(t.Height(), 1);
    auto it = t.Begin();
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.Entry().key, 10);
    EXPECT_EQ(it.Entry().rid, (RID{3, 4}));
}

TEST_F(BPlusTreeTest, DuplicateKeysWithDifferentRidsAreAllowed) {
    BPlusTree t(&bpm, INVALID_PAGE_ID);
    EXPECT_TRUE(t.Insert(5, RID{1, 0}));
    EXPECT_TRUE(t.Insert(5, RID{2, 0}));
    EXPECT_FALSE(t.Insert(5, RID{1, 0})) << "exact (key, rid) duplicate";
    EXPECT_EQ(t.CheckAndCount(), 2u);
}

TEST_F(BPlusTreeTest, RandomInsertsWithSmallFanoutStaySorted) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);  // forces many splits at every level
    std::vector<int64_t> keys;
    for (int i = 0; i < 5000; ++i) keys.push_back(i % 1000);
    std::shuffle(keys.begin(), keys.end(), std::mt19937(7));
    for (size_t i = 0; i < keys.size(); ++i) ASSERT_TRUE(t.Insert(keys[i], RID{static_cast<int>(i), 0}));
    EXPECT_EQ(t.CheckAndCount(), 5000u);
    EXPECT_GT(t.Height(), 4);
    auto sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(Keys(t), sorted);
}

TEST_F(BPlusTreeTest, LowerBoundSemantics) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);
    for (int64_t k = 0; k < 100; k += 10) t.Insert(k, RID{static_cast<int>(k), 0});
    EXPECT_EQ(t.LowerBound(30).Entry().key, 30);
    EXPECT_EQ(t.LowerBound(31).Entry().key, 40);
    EXPECT_EQ(t.LowerBound(-100).Entry().key, 0);
    EXPECT_FALSE(t.LowerBound(91).Valid());
}

TEST_F(BPlusTreeTest, LowerBoundFindsAllDuplicatesAcrossLeaves) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);
    for (int i = 0; i < 50; ++i) t.Insert(7, RID{i, 0});  // spans many leaves
    for (int i = 0; i < 50; ++i) t.Insert(i < 25 ? 6 : 8, RID{i, 1});
    int n = 0;
    for (auto it = t.LowerBound(7); it.Valid() && it.Entry().key == 7; it.Next()) ++n;
    EXPECT_EQ(n, 50);
}

TEST_F(BPlusTreeTest, RemoveIsExactAndLazy) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);
    for (int i = 0; i < 1000; ++i) t.Insert(i, RID{i, 0});
    for (int i = 0; i < 1000; i += 2) EXPECT_TRUE(t.Remove(i, RID{i, 0}));
    EXPECT_FALSE(t.Remove(0, RID{0, 0}));
    EXPECT_FALSE(t.Remove(1, RID{999, 0})) << "right key, wrong rid";
    EXPECT_EQ(t.CheckAndCount(), 500u);
    for (int64_t k : Keys(t)) EXPECT_EQ(k % 2, 1);
    // iteration skips leaves that became completely empty
    for (int i = 1; i < 900; i += 2) t.Remove(i, RID{i, 0});
    EXPECT_EQ(t.LowerBound(0).Entry().key, 901);
}

TEST_F(BPlusTreeTest, ContainsKeyWithException) {
    BPlusTree t(&bpm, INVALID_PAGE_ID);
    t.Insert(1, RID{1, 1});
    EXPECT_TRUE(t.ContainsKey(1));
    EXPECT_FALSE(t.ContainsKey(1, RID{1, 1})) << "the only entry is the excluded one";
    t.Insert(1, RID{2, 2});
    EXPECT_TRUE(t.ContainsKey(1, RID{1, 1}));
    EXPECT_FALSE(t.ContainsKey(2));
}

TEST_F(BPlusTreeTest, FullSizeNodesHaveExpectedHeight) {
    BPlusTree t(&bpm, INVALID_PAGE_ID);
    for (int i = 0; i < 100000; ++i) t.Insert(i, RID{i, 0});
    EXPECT_EQ(t.CheckAndCount(), 100000u);
    EXPECT_EQ(t.Height(), 3);
    EXPECT_EQ(t.LowerBound(77777).Entry().key, 77777);
}

TEST_F(BPlusTreeTest, DescendingInsertOrder) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 5, 5);
    for (int i = 3000; i > 0; --i) t.Insert(i, RID{i, 0});
    EXPECT_EQ(t.CheckAndCount(), 3000u);
    EXPECT_EQ(Keys(t).front(), 1);
}

TEST_F(BPlusTreeTest, ExtremeKeys) {
    BPlusTree t(&bpm, INVALID_PAGE_ID);
    t.Insert(INT64_MIN, RID{1, 0});
    t.Insert(INT64_MAX, RID{2, 0});
    t.Insert(0, RID{3, 0});
    t.Insert(-5, RID{4, 0});
    EXPECT_EQ(Keys(t), (std::vector<int64_t>{INT64_MIN, -5, 0, INT64_MAX}));
    int64_t lo, hi;
    ASSERT_TRUE(t.MinKey(&lo));
    ASSERT_TRUE(t.MaxKey(&hi));
    EXPECT_EQ(lo, INT64_MIN);
    EXPECT_EQ(hi, INT64_MAX);
}

TEST_F(BPlusTreeTest, PersistsThroughRootPage) {
    page_id_t root;
    {
        BPlusTree t(&bpm, INVALID_PAGE_ID, 8, 8);
        for (int i = 0; i < 2000; ++i) t.Insert(i * 3, RID{i, 0});
        root = t.Root();
        bpm.FlushAll();
    }
    BufferPoolManager fresh(16, &disk);  // cold cache: everything comes from disk
    BPlusTree t(&fresh, root, 8, 8);
    EXPECT_EQ(t.CheckAndCount(), 2000u);
    EXPECT_EQ(t.LowerBound(301).Entry().key, 303);
}

TEST_F(BPlusTreeTest, NoPinnedPagesLeftBehind) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);
    for (int i = 0; i < 2000; ++i) t.Insert(i, RID{i, 0});
    for (auto it = t.LowerBound(100); it.Valid() && it.Entry().key < 200; it.Next()) {
    }
    t.Remove(5, RID{5, 0});
    EXPECT_EQ(bpm.PinnedFrames(), 0u);
}

}  // namespace
}  // namespace minidb
