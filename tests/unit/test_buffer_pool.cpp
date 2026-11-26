// Unit tests: BufferPoolManager + PageGuard (caching, pinning, eviction, write-back).
#include <fstream>

#include "buffer_pool.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

class BufferPoolTest : public ::testing::Test {
protected:
    TempFile file;
    DiskManager disk{file.path()};

    // Creates n pages, each containing the text "page-<i>", and unpins them.
    std::vector<page_id_t> MakePages(BufferPoolManager& bpm, int n) {
        std::vector<page_id_t> ids;
        for (int i = 0; i < n; ++i) {
            page_id_t pid;
            PageGuard g = PageGuard::New(&bpm, &pid);
            std::snprintf(g.Data(), 32, "page-%d", i);
            g.MarkDirty();
            ids.push_back(pid);
        }
        return ids;
    }
};

TEST_F(BufferPoolTest, RejectsTinyPool) { EXPECT_THROW(BufferPoolManager(4, &disk), DbError); }

// A page that fails its checksum must not cost a frame: fetching it over and
// over (more often than the pool has frames) keeps failing with the same error
// and never exhausts the pool.
TEST(BufferPoolCorruption, FailedReadDoesNotLeakAFrame) {
    TempFile file;
    {
        DiskManager disk(file.path());
        for (int i = 0; i < 12; ++i) disk.AllocatePage();
    }
    {
        std::fstream raw(file.path(), std::ios::in | std::ios::out | std::ios::binary);
        raw.seekp(static_cast<std::streamoff>(3 * DISK_PAGE_SIZE + 100));
        raw.put('X');
    }
    DiskManager disk(file.path());
    BufferPoolManager bpm(8, &disk);
    for (int attempt = 0; attempt < 20; ++attempt) {
        try {
            bpm.FetchPage(3);
            FAIL() << "page 3 is damaged";
        } catch (const DbError& e) {
            ASSERT_NE(std::string(e.what()).find("checksum"), std::string::npos) << e.what();
        }
    }
    EXPECT_EQ(bpm.PinnedFrames(), 0u);
    std::vector<PageGuard> all;  // every frame is still usable
    for (page_id_t pid : {0, 1, 2, 4, 5, 6, 7, 8}) all.push_back(PageGuard::Fetch(&bpm, pid));
    EXPECT_EQ(bpm.PinnedFrames(), 8u);
}

TEST_F(BufferPoolTest, NewPageIsPinnedZeroedAndAllocatedOnDisk) {
    BufferPoolManager bpm(8, &disk);
    page_id_t pid;
    Page* p = bpm.NewPage(&pid);
    EXPECT_EQ(pid, 0);
    EXPECT_EQ(p->PinCount(), 1);
    EXPECT_EQ(disk.NumPages(), 1);
    for (size_t i = 0; i < PAGE_SIZE; ++i) ASSERT_EQ(p->Data()[i], 0);
    EXPECT_TRUE(bpm.UnpinPage(pid, false));
}

TEST_F(BufferPoolTest, CountsHitsAndMisses) {
    auto ids = [&] {
        BufferPoolManager setup(8, &disk);
        return MakePages(setup, 2);
    }();
    BufferPoolManager bpm(8, &disk);
    { PageGuard g = PageGuard::Fetch(&bpm, ids[0]); }  // miss
    { PageGuard g = PageGuard::Fetch(&bpm, ids[0]); }  // hit
    { PageGuard g = PageGuard::Fetch(&bpm, ids[1]); }  // miss
    EXPECT_EQ(bpm.Misses(), 2u);
    EXPECT_EQ(bpm.Hits(), 1u);
}

TEST_F(BufferPoolTest, EvictsAndWritesBackDirtyPages) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 20);  // more pages than frames
    EXPECT_GE(bpm.Evictions(), 12u);
    for (int i = 0; i < 20; ++i) {
        PageGuard g = PageGuard::Fetch(&bpm, ids[i]);
        EXPECT_STREQ(g.Data(), ("page-" + std::to_string(i)).c_str());
    }
}

TEST_F(BufferPoolTest, CleanPagesAreNotWrittenOnEviction) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 16);
    bpm.FlushAll();
    uint64_t writes = disk.Writes();
    for (int round = 0; round < 3; ++round)
        for (page_id_t pid : ids) PageGuard g = PageGuard::Fetch(&bpm, pid);  // read-only
    EXPECT_EQ(disk.Writes(), writes);
    EXPECT_GT(bpm.Evictions(), 0u);
}

TEST_F(BufferPoolTest, EvictsTheLeastRecentlyUsedPage) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 8);  // fills all frames, unpinned in order 0..7
    { PageGuard g = PageGuard::Fetch(&bpm, ids[0]); }  // 0 becomes most recent
    page_id_t extra;
    { PageGuard g = PageGuard::New(&bpm, &extra); }  // must evict page 1
    bpm.ResetStats();
    { PageGuard g = PageGuard::Fetch(&bpm, ids[0]); }
    EXPECT_EQ(bpm.Hits(), 1u) << "page 0 was recently used and must still be cached";
    { PageGuard g = PageGuard::Fetch(&bpm, ids[1]); }
    EXPECT_EQ(bpm.Misses(), 1u) << "page 1 was the LRU victim";
}

TEST_F(BufferPoolTest, ThrowsWhenAllFramesArePinned) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 9);
    std::vector<PageGuard> pinned;
    for (int i = 0; i < 8; ++i) pinned.push_back(PageGuard::Fetch(&bpm, ids[i]));
    EXPECT_EQ(bpm.PinnedFrames(), 8u);
    EXPECT_THROW(PageGuard::Fetch(&bpm, ids[8]), DbError);
    page_id_t pid;
    EXPECT_THROW(bpm.NewPage(&pid), DbError);
    pinned.pop_back();  // free one frame
    EXPECT_NO_THROW(PageGuard::Fetch(&bpm, ids[8]));
}

TEST_F(BufferPoolTest, PinCountingAcrossMultipleFetches) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 1);
    Page* a = bpm.FetchPage(ids[0]);
    Page* b = bpm.FetchPage(ids[0]);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a->PinCount(), 2);
    EXPECT_TRUE(bpm.UnpinPage(ids[0], false));
    EXPECT_TRUE(bpm.UnpinPage(ids[0], false));
    EXPECT_FALSE(bpm.UnpinPage(ids[0], false)) << "unpin below zero must be rejected";
    EXPECT_FALSE(bpm.UnpinPage(12345, false)) << "unknown page";
}

TEST_F(BufferPoolTest, PageGuardReleasesOnScopeExitAndMove) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 1);
    {
        PageGuard g = PageGuard::Fetch(&bpm, ids[0]);
        EXPECT_EQ(bpm.PinnedFrames(), 1u);
        PageGuard moved = std::move(g);
        EXPECT_EQ(bpm.PinnedFrames(), 1u);
        moved.Release();
        EXPECT_EQ(bpm.PinnedFrames(), 0u);
    }
    EXPECT_EQ(bpm.PinnedFrames(), 0u);
}

TEST_F(BufferPoolTest, FlushAllPersistsToDisk) {
    std::vector<page_id_t> ids;
    {
        BufferPoolManager bpm(8, &disk);
        ids = MakePages(bpm, 3);
        bpm.FlushAll();
    }
    char buf[PAGE_SIZE];
    disk.ReadPage(ids[2], buf);
    EXPECT_STREQ(buf, "page-2");
}

TEST_F(BufferPoolTest, DestructorFlushesDirtyPages) {
    page_id_t pid;
    {
        BufferPoolManager bpm(8, &disk);
        PageGuard g = PageGuard::New(&bpm, &pid);
        std::strcpy(g.Data(), "written by destructor");
        g.MarkDirty();
    }
    char buf[PAGE_SIZE];
    disk.ReadPage(pid, buf);
    EXPECT_STREQ(buf, "written by destructor");
}

TEST_F(BufferPoolTest, DeletedPagesAreReusedLowestFirstAndZeroed) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 5);  // pages 0..4, still cached and dirty
    bpm.DeletePage(ids[3]);
    bpm.DeletePage(ids[1]);
    EXPECT_EQ(bpm.FreePages(), (std::set<page_id_t>{ids[1], ids[3]}));

    page_id_t pid;
    {
        PageGuard g = PageGuard::New(&bpm, &pid);
        EXPECT_EQ(pid, ids[1]) << "lowest free page first";
        for (size_t i = 0; i < PAGE_SIZE; ++i) ASSERT_EQ(g.Data()[i], 0) << "old contents must not leak";
    }
    PageGuard::New(&bpm, &pid);
    EXPECT_EQ(pid, ids[3]);
    PageGuard::New(&bpm, &pid);
    EXPECT_EQ(pid, 5) << "free list exhausted: the file grows again";
    EXPECT_EQ(disk.NumPages(), 6);
    EXPECT_TRUE(bpm.FreePages().empty());
}

TEST_F(BufferPoolTest, DeletedDirtyPageIsNotWrittenBack) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 2);
    bpm.FlushAll();
    {
        PageGuard g = PageGuard::Fetch(&bpm, ids[1]);
        std::strcpy(g.Data(), "garbage");
        g.MarkDirty();
    }
    uint64_t writes = disk.Writes();
    bpm.DeletePage(ids[1]);
    bpm.FlushAll();
    EXPECT_EQ(disk.Writes(), writes) << "a freed page needs no write-back";
    EXPECT_EQ(bpm.PinnedFrames(), 0u);
}

TEST_F(BufferPoolTest, DeletePageRejectsMisuse) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 3);
    PageGuard pinned = PageGuard::Fetch(&bpm, ids[1]);
    EXPECT_THROW(bpm.DeletePage(ids[1]), DbError) << "pinned";
    EXPECT_THROW(bpm.DeletePage(0), DbError) << "the catalog page is never freed";
    EXPECT_THROW(bpm.DeletePage(99), DbError) << "beyond the end of the file";
    bpm.DeletePage(ids[2]);
    EXPECT_THROW(bpm.DeletePage(ids[2]), DbError) << "double free";
}

TEST_F(BufferPoolTest, FreedFramesAreReusedWhenThePoolIsFull) {
    BufferPoolManager bpm(8, &disk);
    std::vector<PageGuard> held;
    for (int i = 0; i < 8; ++i) {
        page_id_t pid;
        held.push_back(PageGuard::New(&bpm, &pid));
    }
    page_id_t victim = held[7].Id();
    held[7].Release();
    bpm.DeletePage(victim);
    page_id_t pid;
    EXPECT_NO_THROW(PageGuard::New(&bpm, &pid)) << "the freed frame is available again";
    EXPECT_EQ(pid, victim);
}

TEST_F(BufferPoolTest, DiscardAllDropsDirtyPagesWithoutWritingThem) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 3);
    bpm.FlushAll();
    {
        PageGuard g = PageGuard::Fetch(&bpm, ids[1]);
        std::strcpy(g.Data(), "changed");
        g.MarkDirty();
    }
    const uint64_t writes = disk.Writes();
    bpm.DiscardAll();
    EXPECT_EQ(disk.Writes(), writes) << "nothing is written back";
    {
        PageGuard g = PageGuard::Fetch(&bpm, ids[1]);
        EXPECT_STREQ(g.Data(), "page-1") << "the cached change is gone; the page comes from disk again";
    }
    std::vector<PageGuard> held;  // and all 8 frames are usable again
    for (int i = 0; i < 8; ++i) {
        page_id_t pid;
        held.push_back(PageGuard::New(&bpm, &pid));
    }
}

TEST_F(BufferPoolTest, DiscardAllRefusesWhilePagesArePinned) {
    BufferPoolManager bpm(8, &disk);
    auto ids = MakePages(bpm, 2);
    PageGuard g = PageGuard::Fetch(&bpm, ids[0]);
    EXPECT_THROW(bpm.DiscardAll(), DbError);
}

}  // namespace
}  // namespace minidb
