// Unit tests: BPlusTree (disk-based index).
#include <algorithm>
#include <climits>
#include <random>
#include <set>

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

TEST_F(BPlusTreeTest, RemoveIsExact) {
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

// Regression (found by the differential test below): emptying the rightmost
// leaves made MaxKey report "no statistics" although the tree still had entries.
TEST_F(BPlusTreeTest, MinMaxKeySkipEmptiedLeaves) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);
    for (int i = 0; i < 200; ++i) t.Insert(i, RID{i, 0});
    for (int i = 150; i < 200; ++i) t.Remove(i, RID{i, 0});  // many rightmost leaves become empty
    for (int i = 0; i < 50; ++i) t.Remove(i, RID{i, 0});     // and many leftmost ones
    int64_t mn = 0, mx = 0;
    ASSERT_TRUE(t.MinKey(&mn));
    ASSERT_TRUE(t.MaxKey(&mx));
    EXPECT_EQ(mn, 50);
    EXPECT_EQ(mx, 149);
    for (int i = 50; i < 150; ++i) t.Remove(i, RID{i, 0});
    EXPECT_FALSE(t.MinKey(&mn));
    EXPECT_FALSE(t.MaxKey(&mx)) << "a tree whose entries were all deleted has no max";
}

// Differential test: a long random mix of inserts and removes (with many
// duplicate keys and small nodes, so splits and emptied leaves are frequent)
// must always agree with std::set on every query the tree supports.
TEST_F(BPlusTreeTest, RandomOperationsMatchReferenceModel) {
    for (uint32_t seed = 1; seed <= 5; ++seed) {
        BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);
        std::set<IndexKey> ref;
        std::mt19937 rng(seed);
        auto random_entry = [&] {
            return IndexKey{static_cast<int64_t>(rng() % 60) - 30, RID{static_cast<page_id_t>(rng() % 8), static_cast<uint32_t>(rng() % 4)}};
        };
        for (int step = 0; step < 4000; ++step) {
            IndexKey k = random_entry();
            // Phases: grow, then mostly shrink (empties whole leaves), then grow again.
            bool insert = (step / 1000) % 2 == 0 ? rng() % 4 != 0 : rng() % 4 == 0;
            if (insert) {
                ASSERT_EQ(t.Insert(k.key, k.rid), ref.insert(k).second) << "seed " << seed << " step " << step;
            } else {
                ASSERT_EQ(t.Remove(k.key, k.rid), ref.erase(k) == 1) << "seed " << seed << " step " << step;
            }
            if (step % 97 != 0) continue;

            ASSERT_EQ(t.CheckAndCount(), ref.size());
            ASSERT_EQ(t.CheckStructure(), ref.size()) << "seed " << seed << " step " << step;
            std::vector<IndexKey> all;
            for (auto it = t.Begin(); it.Valid(); it.Next()) all.push_back(it.Entry());
            ASSERT_TRUE(std::equal(all.begin(), all.end(), ref.begin(), ref.end())) << "seed " << seed;
            for (int64_t q = -32; q <= 32; q += 3) {
                auto it = t.LowerBound(q);
                auto rit = ref.lower_bound(IndexKey{q, RID{INT32_MIN, 0}});
                ASSERT_EQ(it.Valid(), rit != ref.end()) << "LowerBound(" << q << ")";
                if (it.Valid()) {
                    ASSERT_EQ(it.Entry(), *rit) << "LowerBound(" << q << ")";
                }
                bool any = rit != ref.end() && rit->key == q;
                ASSERT_EQ(t.ContainsKey(q), any) << "ContainsKey(" << q << ")";
            }
            int64_t mn = 0, mx = 0;
            ASSERT_EQ(t.MinKey(&mn), !ref.empty());
            ASSERT_EQ(t.MaxKey(&mx), !ref.empty());
            if (!ref.empty()) {
                EXPECT_EQ(mn, ref.begin()->key);
                EXPECT_EQ(mx, ref.rbegin()->key);
            }
        }
        EXPECT_EQ(bpm.PinnedFrames(), 0u);
    }
}

TEST_F(BPlusTreeTest, CorruptNodeHeaderIsAnErrorNotACrash) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);
    for (int i = 0; i < 100; ++i) t.Insert(i, RID{i, 0});
    {
        PageGuard g = PageGuard::Fetch(&bpm, t.Root());
        WriteAs<uint16_t>(g.Data() + 2, 30000);  // entry count far beyond the page
        g.MarkDirty();
    }
    EXPECT_THROW(t.LowerBound(5), DbError);
    EXPECT_THROW(t.Insert(1000, RID{1, 1}), DbError);
    {
        PageGuard g = PageGuard::Fetch(&bpm, t.Root());
        WriteAs<uint16_t>(g.Data() + 2, 2);
        g.Data()[0] = 7;  // neither leaf (1) nor internal (0)
        g.MarkDirty();
    }
    EXPECT_THROW(t.ContainsKey(5), DbError);
    EXPECT_EQ(bpm.PinnedFrames(), 0u) << "errors must not leak pins";
}

TEST_F(BPlusTreeTest, CycleInLeafChainIsAnErrorNotAHang) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);
    for (int i = 0; i < 100; ++i) t.Insert(i, RID{i, 0});
    // Find the last leaf by walking the chain, then point it back to the start.
    std::vector<page_id_t> leaves;
    for (page_id_t p : t.Pages()) {
        PageGuard g = PageGuard::Fetch(&bpm, p);
        if (g.Data()[0] == 1) leaves.push_back(p);
    }
    for (page_id_t p : leaves) {
        PageGuard g = PageGuard::Fetch(&bpm, p);
        if (ReadAs<page_id_t>(g.Data() + 4) == INVALID_PAGE_ID) {
            WriteAs<page_id_t>(g.Data() + 4, leaves.front());
            g.MarkDirty();
        }
    }
    auto walk = [&] {
        for (auto it = t.Begin(); it.Valid(); it.Next()) {
        }
    };
    EXPECT_THROW(walk(), DbError);
    EXPECT_EQ(bpm.PinnedFrames(), 0u);
}

// ---- wide trees: (key, key2, rid) entries, for multi-column indexes ----

TEST_F(BPlusTreeTest, WideTreeOrdersByBothKeyParts) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, BPlusTree::LEAF_CAPACITY, BPlusTree::INTERNAL_CAPACITY, true);
    EXPECT_TRUE(t.Wide());
    EXPECT_EQ(t.LeafCapacity(), BPlusTree::WIDE_LEAF_CAPACITY);
    // Every (key, key2) pair in [0,30) x [-15,15), inserted in a scrambled order.
    std::vector<IndexKey> in;
    for (int64_t k = 0; k < 30; ++k)
        for (int64_t k2 = -15; k2 < 15; ++k2) in.push_back(IndexKey{k, RID{static_cast<page_id_t>(k), 0}, k2});
    std::shuffle(in.begin(), in.end(), std::mt19937(7));
    for (const auto& k : in) ASSERT_TRUE(t.Insert(k));
    EXPECT_FALSE(t.Insert(in[0])) << "exact duplicate entry";
    EXPECT_TRUE(t.Insert(IndexKey{in[0].key, RID{999, 0}, in[0].key2})) << "same key pair, other row";
    ASSERT_TRUE(t.Remove(IndexKey{in[0].key, RID{999, 0}, in[0].key2}));
    EXPECT_GT(t.Height(), 1) << "900 wide entries need more than one leaf";
    EXPECT_EQ(t.CheckAndCount(), 900u);

    std::sort(in.begin(), in.end());
    std::vector<IndexKey> out;
    for (auto it = t.Begin(); it.Valid(); it.Next()) out.push_back(it.Entry());
    EXPECT_EQ(out, in) << "leaf chain is sorted by key, then key2";

    EXPECT_EQ(t.LowerBound(7, 3).Entry().key, 7);
    EXPECT_EQ(t.LowerBound(7, 3).Entry().key2, 3);
    EXPECT_EQ(t.LowerBound(7).Entry().key2, -15) << "without key2: the first entry of the key";
    auto next = t.LowerBound(7, 15);  // past the last key2 of 7
    EXPECT_EQ(next.Entry().key, 8);
    EXPECT_EQ(next.Entry().key2, -15);
    EXPECT_FALSE(t.LowerBound(29, 15).Valid());
    EXPECT_FALSE(t.Remove(IndexKey{7, RID{7, 0}, 99})) << "key2 is part of the entry";
    EXPECT_TRUE(t.Remove(IndexKey{7, RID{7, 0}, 3}));
    EXPECT_EQ(t.LowerBound(7, 3).Entry().key2, 4);
    EXPECT_EQ(bpm.PinnedFrames(), 0u);
}

TEST_F(BPlusTreeTest, WideTreeRandomOperationsMatchReferenceModel) {
    for (uint32_t seed = 1; seed <= 4; ++seed) {
        BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4, true);  // tiny nodes: many splits
        std::set<IndexKey> ref;
        std::mt19937 rng(seed);
        auto random_entry = [&] {
            return IndexKey{static_cast<int64_t>(rng() % 12) - 6,
                            RID{static_cast<page_id_t>(rng() % 4), static_cast<uint32_t>(rng() % 3)},
                            static_cast<int64_t>(rng() % 10) - 5};
        };
        for (int step = 0; step < 3000; ++step) {
            IndexKey k = random_entry();
            bool insert = (step / 1000) % 2 == 0 ? rng() % 4 != 0 : rng() % 4 == 0;
            if (insert)
                ASSERT_EQ(t.Insert(k), ref.insert(k).second) << "seed " << seed << " step " << step;
            else
                ASSERT_EQ(t.Remove(k), ref.erase(k) == 1) << "seed " << seed << " step " << step;
            if (step % 89 != 0) continue;
            ASSERT_EQ(t.CheckAndCount(), ref.size());
            ASSERT_EQ(t.CheckStructure(), ref.size()) << "seed " << seed << " step " << step;
            for (int64_t q = -7; q <= 7; ++q) {
                for (int64_t q2 = -6; q2 <= 6; q2 += 3) {
                    auto it = t.LowerBound(q, q2);
                    auto rit = ref.lower_bound(IndexKey{q, RID{INT32_MIN, 0}, q2});
                    ASSERT_EQ(it.Valid(), rit != ref.end()) << "LowerBound(" << q << ", " << q2 << ")";
                    if (it.Valid()) {
                        ASSERT_EQ(it.Entry(), *rit) << "LowerBound(" << q << ", " << q2 << ")";
                    }
                }
            }
        }
        EXPECT_EQ(bpm.PinnedFrames(), 0u);
    }
}

TEST_F(BPlusTreeTest, NarrowTreeHasNoSecondKeyPart) {
    BPlusTree t(&bpm, INVALID_PAGE_ID);
    EXPECT_FALSE(t.Wide());
    EXPECT_THROW(t.Insert(IndexKey{1, RID{1, 1}, 5}), DbError) << "a single-key tree cannot store key2";
    t.Insert(1, RID{1, 1});
    EXPECT_EQ(t.Begin().Entry().key2, 0);
}

TEST_F(BPlusTreeTest, LayoutMismatchIsDetected) {
    // A node records whether it is wide; reading it with the other layout would
    // misinterpret every entry, so it must be reported as corruption.
    BPlusTree wide(&bpm, INVALID_PAGE_ID, 4, 4, true);
    BPlusTree narrow(&bpm, INVALID_PAGE_ID, 4, 4, false);
    for (int i = 0; i < 50; ++i) {
        wide.Insert(IndexKey{i, RID{i, 0}, i});
        narrow.Insert(i, RID{i, 0});
    }
    BPlusTree wide_as_narrow(&bpm, wide.Root());
    BPlusTree narrow_as_wide(&bpm, narrow.Root(), 4, 4, true);
    EXPECT_THROW(wide_as_narrow.LowerBound(5), DbError);
    EXPECT_THROW(narrow_as_wide.CheckAndCount(), DbError);
    EXPECT_EQ(wide.CheckAndCount(), 50u);
    EXPECT_EQ(bpm.PinnedFrames(), 0u);
}

// ---- rebalancing on delete ----

TEST_F(BPlusTreeTest, DeletingEverythingShrinksTheTreeToOneLeaf) {
    for (int fanout : {4, 5, 7}) {
        BPlusTree t(&bpm, INVALID_PAGE_ID, fanout, fanout);
        for (int i = 0; i < 2000; ++i) t.Insert(i, RID{i, 0});
        const size_t pages_before = t.Pages().size();
        const size_t free_before = bpm.FreePages().size();
        EXPECT_GE(t.Height(), 4);
        for (int i = 0; i < 2000; ++i) {
            ASSERT_TRUE(t.Remove(i, RID{i, 0}));
            if (i % 101 == 0) ASSERT_EQ(t.CheckStructure(), static_cast<size_t>(1999 - i)) << "after removing " << i;
        }
        EXPECT_EQ(t.Height(), 1) << "fanout " << fanout;
        EXPECT_EQ(t.Pages().size(), 1u);
        EXPECT_EQ(bpm.FreePages().size() - free_before, pages_before - 1) << "every other node was freed";
        EXPECT_FALSE(t.Begin().Valid());
        EXPECT_EQ(bpm.PinnedFrames(), 0u);
    }
}

TEST_F(BPlusTreeTest, DeletingFromTheMiddleAndBothEndsKeepsTheTreeBalanced) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);
    std::set<int64_t> live;
    for (int i = 0; i < 1500; ++i) {
        t.Insert(i, RID{i, 0});
        live.insert(i);
    }
    auto remove = [&](int i) {
        ASSERT_TRUE(t.Remove(i, RID{i, 0}));
        live.erase(i);
    };
    for (int i = 500; i < 1000; ++i) remove(i);   // a hole in the middle
    for (int i = 0; i < 200; ++i) remove(i);      // the left edge (no left siblings)
    for (int i = 1499; i >= 1300; --i) remove(i);  // the right edge (no right siblings)
    EXPECT_EQ(t.CheckStructure(), live.size());
    std::vector<int64_t> keys = Keys(t);
    EXPECT_TRUE(std::equal(keys.begin(), keys.end(), live.begin(), live.end()));
    EXPECT_EQ(t.LowerBound(500).Entry().key, 1000);
    int64_t mx = 0;
    ASSERT_TRUE(t.MaxKey(&mx));
    EXPECT_EQ(mx, 1299);
}

TEST_F(BPlusTreeTest, FreedNodesAreReusedInsteadOfGrowingTheFile) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 4, 4);
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 1000; ++i) t.Insert(i, RID{i, 0});
        for (int i = 0; i < 1000; ++i) t.Remove(i, RID{i, 0});
    }
    const page_id_t pages_after_3_rounds = disk.NumPages();
    for (int i = 0; i < 1000; ++i) t.Insert(i, RID{i, 0});
    for (int i = 0; i < 1000; ++i) t.Remove(i, RID{i, 0});
    EXPECT_EQ(disk.NumPages(), pages_after_3_rounds);
    EXPECT_EQ(t.CheckStructure(), 0u);
}

TEST_F(BPlusTreeTest, ManyDuplicatesOfOneKeyRebalance) {
    BPlusTree t(&bpm, INVALID_PAGE_ID, 5, 4, true);
    for (int i = 0; i < 800; ++i) t.Insert(IndexKey{7, RID{i, 0}, i % 3});
    for (int i = 0; i < 800; i += 2) ASSERT_TRUE(t.Remove(IndexKey{7, RID{i, 0}, i % 3}));
    EXPECT_EQ(t.CheckStructure(), 400u);
    size_t n = 0;
    for (auto it = t.LowerBound(7); it.Valid(); it.Next()) ++n;
    EXPECT_EQ(n, 400u);
    EXPECT_TRUE(t.ContainsKey(7));
}

TEST_F(BPlusTreeTest, RebalancedTreeSurvivesAReload) {
    page_id_t root;
    {
        BPlusTree t(&bpm, INVALID_PAGE_ID, 6, 5);
        for (int i = 0; i < 3000; ++i) t.Insert(i * 7 % 3000, RID{i, 0});
        for (int i = 0; i < 3000; ++i)
            if (i % 5 != 0) t.Remove(i * 7 % 3000, RID{i, 0});
        root = t.Root();
        bpm.FlushAll();
    }
    BufferPoolManager fresh(16, &disk);
    BPlusTree t(&fresh, root, 6, 5);
    EXPECT_EQ(t.CheckStructure(), 600u);
    EXPECT_EQ(fresh.PinnedFrames(), 0u);
}

}  // namespace
}  // namespace minidb
