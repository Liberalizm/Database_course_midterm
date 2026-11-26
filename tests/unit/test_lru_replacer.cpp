// Unit tests: LRUReplacer (eviction policy, independent of any pages).
#include "buffer_pool.h"
#include "test_util.h"

namespace minidb {
namespace {

TEST(LRUReplacer, EmptyHasNoVictim) {
    LRUReplacer r;
    frame_id_t f;
    EXPECT_FALSE(r.Victim(&f));
    EXPECT_EQ(r.Size(), 0u);
}

TEST(LRUReplacer, EvictsInLeastRecentlyUnpinnedOrder) {
    LRUReplacer r;
    for (frame_id_t f : {3, 1, 2}) r.Unpin(f);
    frame_id_t v;
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 3);
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 2);
    EXPECT_FALSE(r.Victim(&v));
}

TEST(LRUReplacer, PinnedFrameIsNeverEvicted) {
    LRUReplacer r;
    r.Unpin(1);
    r.Unpin(2);
    r.Pin(1);
    EXPECT_EQ(r.Size(), 1u);
    frame_id_t v;
    ASSERT_TRUE(r.Victim(&v));
    EXPECT_EQ(v, 2);
    EXPECT_FALSE(r.Victim(&v));
}

TEST(LRUReplacer, RepeatedUnpinDoesNotRefreshOrDuplicate) {
    LRUReplacer r;
    r.Unpin(1);
    r.Unpin(2);
    r.Unpin(1);  // already evictable: no-op
    EXPECT_EQ(r.Size(), 2u);
    frame_id_t v;
    r.Victim(&v);
    EXPECT_EQ(v, 1);
}

TEST(LRUReplacer, PinOfUnknownFrameIsHarmless) {
    LRUReplacer r;
    r.Pin(42);
    EXPECT_EQ(r.Size(), 0u);
}

TEST(LRUReplacer, RePinAndUnpinMovesToMostRecent) {
    LRUReplacer r;
    r.Unpin(1);
    r.Unpin(2);
    r.Pin(1);
    r.Unpin(1);  // 1 is now the most recently used
    frame_id_t v;
    r.Victim(&v);
    EXPECT_EQ(v, 2);
}

}  // namespace
}  // namespace minidb
