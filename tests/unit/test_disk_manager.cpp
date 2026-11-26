// Unit tests: DiskManager (raw page I/O on a single file).
#include "disk_manager.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

TEST(DiskManager, NewFileHasNoPages) {
    TempFile f;
    DiskManager disk(f.path());
    EXPECT_EQ(disk.NumPages(), 0);
    EXPECT_EQ(disk.Reads(), 0u);
    EXPECT_EQ(disk.Writes(), 0u);
}

TEST(DiskManager, AllocateReturnsSequentialZeroedPages) {
    TempFile f;
    DiskManager disk(f.path());
    for (page_id_t expected = 0; expected < 5; ++expected) EXPECT_EQ(disk.AllocatePage(), expected);
    EXPECT_EQ(disk.NumPages(), 5);
    char buf[PAGE_SIZE];
    std::memset(buf, 0xAB, PAGE_SIZE);
    disk.ReadPage(3, buf);
    for (size_t i = 0; i < PAGE_SIZE; ++i) ASSERT_EQ(buf[i], 0) << "byte " << i;
}

TEST(DiskManager, WriteThenReadRoundTrip) {
    TempFile f;
    DiskManager disk(f.path());
    disk.AllocatePage();
    page_id_t pid = disk.AllocatePage();
    char out[PAGE_SIZE], in[PAGE_SIZE];
    for (size_t i = 0; i < PAGE_SIZE; ++i) out[i] = static_cast<char>(i * 7);
    disk.WritePage(pid, out);
    disk.ReadPage(pid, in);
    EXPECT_EQ(std::memcmp(in, out, PAGE_SIZE), 0);
    disk.ReadPage(0, in);  // neighbour page untouched
    EXPECT_EQ(in[0], 0);
}

TEST(DiskManager, ContentSurvivesReopen) {
    TempFile f;
    {
        DiskManager disk(f.path());
        for (int i = 0; i < 3; ++i) disk.AllocatePage();
        char buf[PAGE_SIZE] = {};
        std::strcpy(buf, "persisted page two");
        disk.WritePage(2, buf);
    }
    DiskManager disk(f.path());
    EXPECT_EQ(disk.NumPages(), 3);
    char buf[PAGE_SIZE];
    disk.ReadPage(2, buf);
    EXPECT_STREQ(buf, "persisted page two");
}

TEST(DiskManager, InvalidPageIdsThrow) {
    TempFile f;
    DiskManager disk(f.path());
    disk.AllocatePage();
    char buf[PAGE_SIZE] = {};
    EXPECT_THROW(disk.ReadPage(-1, buf), DbError);
    EXPECT_THROW(disk.ReadPage(1, buf), DbError);
    EXPECT_THROW(disk.WritePage(7, buf), DbError);
}

TEST(DiskManager, CountsIo) {
    TempFile f;
    DiskManager disk(f.path());
    disk.AllocatePage();  // allocation writes nothing: the page is zeros until written
    EXPECT_EQ(disk.Writes(), 0u);
    char buf[PAGE_SIZE] = {};
    disk.WritePage(0, buf);
    EXPECT_EQ(disk.Writes(), 1u);
    disk.ReadPage(0, buf);
    disk.ReadPage(0, buf);
    EXPECT_EQ(disk.Reads(), 2u);
}

// ---- write-ahead log -------------------------------------------------------

void Fill(char* buf, char c) { std::memset(buf, c, PAGE_SIZE); }

char FirstByte(DiskManager& disk, page_id_t pid) {
    char buf[PAGE_SIZE];
    disk.ReadPage(pid, buf);
    return buf[0];
}

TEST(DiskManager, WritesGoToTheLogNotTheDatabaseFile) {
    TempFile f;
    DiskManager disk(f.path());
    char buf[PAGE_SIZE];
    Fill(buf, 'a');
    disk.AllocatePage();
    disk.WritePage(0, buf);
    disk.Commit();
    EXPECT_EQ(std::filesystem::file_size(f.path()), 0u) << "the database file changes only at a checkpoint";
    EXPECT_EQ(disk.Log().PageRecords(), 1u);
    EXPECT_EQ(FirstByte(disk, 0), 'a') << "reads see the logged version";
    disk.Checkpoint();
    EXPECT_EQ(std::filesystem::file_size(f.path()), PAGE_SIZE);
    EXPECT_EQ(disk.Log().PageRecords(), 0u);
    EXPECT_EQ(disk.Log().SizeBytes(), WriteAheadLog::kHeaderSize);
    EXPECT_EQ(FirstByte(disk, 0), 'a');
}

TEST(DiskManager, RollbackRestoresCommittedPagesAndSize) {
    TempFile f;
    DiskManager disk(f.path());
    char buf[PAGE_SIZE];
    disk.AllocatePage();
    Fill(buf, 'a');
    disk.WritePage(0, buf);
    disk.Commit();

    Fill(buf, 'b');
    disk.WritePage(0, buf);
    disk.AllocatePage();
    disk.WritePage(1, buf);
    EXPECT_TRUE(disk.HasUncommittedChanges());
    disk.Rollback();
    EXPECT_FALSE(disk.HasUncommittedChanges());
    EXPECT_EQ(disk.NumPages(), 1);
    EXPECT_EQ(FirstByte(disk, 0), 'a');
    EXPECT_EQ(disk.AllocatePage(), 1) << "the rolled-back allocation is handed out again";
}

TEST(DiskManager, CommittedWritesSurviveACrash) {
    TempFile f;
    std::string copy = f.path() + ".crash";
    {
        DiskManager disk(f.path());
        char buf[PAGE_SIZE];
        for (int i = 0; i < 3; ++i) disk.AllocatePage();
        Fill(buf, 'x');
        disk.WritePage(2, buf);
        disk.Commit();
        Fill(buf, 'y');  // not committed when the "crash" happens
        disk.WritePage(1, buf);
        disk.AllocatePage();
        testing::CopyCrashImage(f.path(), copy);
    }
    {
        DiskManager disk(copy);
        EXPECT_EQ(disk.RecoveredCommits(), 1u);
        EXPECT_EQ(disk.NumPages(), 3) << "the uncommitted allocation is gone";
        EXPECT_EQ(FirstByte(disk, 2), 'x');
        EXPECT_EQ(FirstByte(disk, 1), 0) << "the uncommitted write is gone";
        EXPECT_EQ(disk.Log().PageRecords(), 0u) << "recovery checkpoints right away";
    }
    std::remove(copy.c_str());
    std::remove((copy + ".wal").c_str());
}

// A crash in the middle of a checkpoint can leave a torn page in the database
// file. The log still holds the full image, and recovery writes it again.
TEST(DiskManager, RecoveryRepairsPagesTornDuringACheckpoint) {
    TempFile f;
    std::string copy = f.path() + ".crash";
    {
        DiskManager disk(f.path());
        char buf[PAGE_SIZE];
        disk.AllocatePage();
        disk.AllocatePage();
        Fill(buf, 'g');
        disk.WritePage(1, buf);
        disk.Commit();
        testing::CopyCrashImage(f.path(), copy);
    }
    std::vector<char> torn(2 * PAGE_SIZE, 0);
    std::memset(torn.data() + PAGE_SIZE, 'g', 100);  // only the start of page 1 made it
    testing::WriteFileBytes(copy, torn);
    {
        DiskManager disk(copy);
        char buf[PAGE_SIZE];
        disk.ReadPage(1, buf);
        for (size_t i = 0; i < PAGE_SIZE; ++i) ASSERT_EQ(buf[i], 'g') << "byte " << i;
    }
    std::remove(copy.c_str());
    std::remove((copy + ".wal").c_str());
}

TEST(DiskManager, CleanShutdownCheckpointsAndRemovesTheLog) {
    TempFile f;
    {
        DiskManager disk(f.path());
        EXPECT_TRUE(std::filesystem::exists(f.wal_path()));
        disk.AllocatePage();
        char buf[PAGE_SIZE];
        Fill(buf, 'q');
        disk.WritePage(0, buf);  // not even committed: a clean shutdown commits it
    }
    EXPECT_FALSE(std::filesystem::exists(f.wal_path()));
    EXPECT_EQ(std::filesystem::file_size(f.path()), PAGE_SIZE);
    DiskManager disk(f.path());
    EXPECT_EQ(FirstByte(disk, 0), 'q');
    EXPECT_EQ(disk.RecoveredCommits(), 0u);
}

TEST(DiskManager, CheckpointsAutomaticallyWhenTheLogGrows) {
    TempFile f;
    DiskManager disk(f.path(), DiskOptions{true, 10});
    char buf[PAGE_SIZE];
    for (int i = 0; i < 30; ++i) {
        page_id_t pid = disk.AllocatePage();
        Fill(buf, static_cast<char>('A' + i));
        disk.WritePage(pid, buf);
        disk.Commit();
        EXPECT_LT(disk.Log().PageRecords(), 10u);
    }
    EXPECT_EQ(disk.Checkpoints(), 3u);
    for (page_id_t pid = 0; pid < 30; ++pid) EXPECT_EQ(FirstByte(disk, pid), static_cast<char>('A' + pid));
}

TEST(DiskManager, CheckpointRefusesUncommittedChanges) {
    TempFile f;
    DiskManager disk(f.path());
    disk.AllocatePage();
    EXPECT_THROW(disk.Checkpoint(), DbError);
    disk.Commit();
    EXPECT_NO_THROW(disk.Checkpoint());
}

TEST(DiskManager, InjectedWriteFailure) {
    TempFile f;
    DiskManager disk(f.path());
    char buf[PAGE_SIZE] = {};
    disk.AllocatePage();
    disk.FailWritesAfter(1);
    EXPECT_NO_THROW(disk.WritePage(0, buf));
    EXPECT_THROW(disk.WritePage(0, buf), DbError);
    EXPECT_THROW(disk.WritePage(0, buf), DbError) << "the disk stays broken";
    disk.FailWritesAfter(-1);
    EXPECT_NO_THROW(disk.WritePage(0, buf));
}

}  // namespace
}  // namespace minidb
