// Unit tests: DiskManager (raw page I/O on a single file, page checksums).
#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

#include "checksum.h"
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

// ---- page checksums ----

// Raw access to the file behind a closed DiskManager, to damage it on purpose.
std::vector<char> ReadFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void WriteFileBytes(const std::string& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Creates a file with n pages; page i is filled with the byte value 'a' + i.
void MakeFile(const std::string& path, int n) {
    DiskManager disk(path);
    for (int i = 0; i < n; ++i) {
        page_id_t pid = disk.AllocatePage();
        char buf[PAGE_SIZE];
        std::memset(buf, 'a' + i, PAGE_SIZE);
        disk.WritePage(pid, buf);
    }
}

TEST(DiskManager, EveryPageIsStoredWithItsChecksum) {
    TempFile f;
    MakeFile(f.path(), 3);
    std::vector<char> bytes = ReadFileBytes(f.path());
    ASSERT_EQ(bytes.size(), 3 * DISK_PAGE_SIZE);
    for (page_id_t pid = 0; pid < 3; ++pid) {
        const char* frame = bytes.data() + static_cast<size_t>(pid) * DISK_PAGE_SIZE;
        EXPECT_EQ(frame[0], 'a' + pid);
        EXPECT_EQ(ReadAs<uint32_t>(frame + PAGE_SIZE), PageChecksum(pid, frame)) << "page " << pid;
    }
}

TEST(DiskManager, DamagedPageFailsChecksumOthersStillReadable) {
    TempFile f;
    MakeFile(f.path(), 3);
    std::vector<char> bytes = ReadFileBytes(f.path());
    char& victim = bytes[1 * DISK_PAGE_SIZE + 1234];
    victim = static_cast<char>(victim ^ 0x10);  // one flipped bit in page 1
    WriteFileBytes(f.path(), bytes);

    DiskManager disk(f.path());
    char buf[PAGE_SIZE];
    try {
        disk.ReadPage(1, buf);
        FAIL() << "expected a checksum error";
    } catch (const DbError& e) {
        EXPECT_NE(std::string(e.what()).find("page 1 failed its checksum"), std::string::npos) << e.what();
    }
    disk.ReadPage(0, buf);
    EXPECT_EQ(buf[0], 'a');
    disk.ReadPage(2, buf);
    EXPECT_EQ(buf[0], 'c');
}

TEST(DiskManager, DamagedChecksumIsDetected) {
    TempFile f;
    MakeFile(f.path(), 2);
    std::vector<char> bytes = ReadFileBytes(f.path());
    char& stored = bytes[1 * DISK_PAGE_SIZE + PAGE_SIZE];  // the stored checksum itself
    stored = static_cast<char>(stored ^ 0x01);
    WriteFileBytes(f.path(), bytes);
    DiskManager disk(f.path());
    char buf[PAGE_SIZE];
    EXPECT_THROW(disk.ReadPage(1, buf), DbError);
}

TEST(DiskManager, PageWrittenToTheWrongPlaceIsDetected) {
    TempFile f;
    MakeFile(f.path(), 3);
    std::vector<char> bytes = ReadFileBytes(f.path());
    // Copy page 0 together with its (valid) checksum over page 2.
    std::copy_n(bytes.begin(), DISK_PAGE_SIZE, bytes.begin() + 2 * DISK_PAGE_SIZE);
    WriteFileBytes(f.path(), bytes);
    DiskManager disk(f.path());
    char buf[PAGE_SIZE];
    EXPECT_THROW(disk.ReadPage(2, buf), DbError);
}

TEST(DiskManager, RewritingAPageUpdatesItsChecksum) {
    TempFile f;
    MakeFile(f.path(), 2);
    {
        DiskManager disk(f.path());
        char buf[PAGE_SIZE] = {};
        std::strcpy(buf, "rewritten");
        disk.WritePage(1, buf);
    }
    DiskManager disk(f.path());
    char buf[PAGE_SIZE];
    disk.ReadPage(1, buf);
    EXPECT_STREQ(buf, "rewritten");
}

TEST(DiskManager, RejectsFileThatIsNotWholePages) {
    TempFile f;
    MakeFile(f.path(), 2);
    std::vector<char> bytes = ReadFileBytes(f.path());
    bytes.resize(bytes.size() - 1);  // truncated
    WriteFileBytes(f.path(), bytes);
    EXPECT_THROW(DiskManager{f.path()}, DbError);

    // A file from a MiniDB version without checksums: pages of exactly PAGE_SIZE bytes.
    WriteFileBytes(f.path(), std::vector<char>(3 * PAGE_SIZE, '\0'));
    EXPECT_THROW(DiskManager{f.path()}, DbError);
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
    EXPECT_EQ(std::filesystem::file_size(f.path()), DISK_PAGE_SIZE);
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
    std::vector<char> torn(2 * DISK_PAGE_SIZE, 0);
    std::memset(torn.data() + DISK_PAGE_SIZE, 'g', 100);  // only the start of page 1 made it
    testing::WriteFileBytes(copy, torn);
    {
        DiskManager disk(copy);
        char buf[PAGE_SIZE];
        disk.ReadPage(1, buf);
        for (size_t i = 0; i < PAGE_SIZE; ++i) ASSERT_EQ(buf[i], 'g') << "byte " << i;
        // Page 0 was never written: the checkpoint only gave it zeros, and the
        // crash left a hole there. Recovery writes it again, checksum and all.
        disk.ReadPage(0, buf);
        for (size_t i = 0; i < PAGE_SIZE; ++i) ASSERT_EQ(buf[i], 0) << "byte " << i;
    }
    {
        DiskManager disk(copy);  // and the repaired file is valid on its own
        char buf[PAGE_SIZE];
        disk.ReadPage(0, buf);
        disk.ReadPage(1, buf);
        EXPECT_EQ(buf[0], 'g');
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
    EXPECT_EQ(std::filesystem::file_size(f.path()), DISK_PAGE_SIZE);
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

TEST(DiskManager, SavepointsUndoWritesAndAllocations) {
    TempFile f;
    DiskManager disk(f.path());
    char buf[PAGE_SIZE];
    disk.AllocatePage();
    Fill(buf, 'a');
    disk.WritePage(0, buf);
    disk.Commit();

    Fill(buf, 'b');
    disk.WritePage(0, buf);
    disk.Savepoint();
    EXPECT_EQ(disk.SavepointDepth(), 1u);
    Fill(buf, 'c');
    disk.WritePage(0, buf);
    EXPECT_EQ(disk.AllocatePage(), 1);
    disk.WritePage(1, buf);
    disk.RollbackToSavepoint();
    EXPECT_EQ(disk.NumPages(), 1) << "the allocation since the savepoint is undone";
    EXPECT_EQ(FirstByte(disk, 0), 'b') << "the write before the savepoint stays";
    EXPECT_EQ(disk.SavepointDepth(), 1u);
    EXPECT_EQ(disk.AllocatePage(), 1);
    disk.ReleaseSavepoint();
    disk.Commit();
    EXPECT_EQ(disk.NumPages(), 2);
    EXPECT_EQ(FirstByte(disk, 0), 'b');
    EXPECT_EQ(FirstByte(disk, 1), 0) << "allocated again, never written";
    EXPECT_THROW(disk.ReleaseSavepoint(), DbError);
}

TEST(DiskManager, CommitEndsTheSavepoints) {
    TempFile f;
    DiskManager disk(f.path());
    disk.Savepoint();
    disk.Savepoint();
    disk.Commit();  // nothing to commit, but the savepoints still end
    EXPECT_EQ(disk.SavepointDepth(), 0u);
    EXPECT_EQ(disk.Log().Marks(), 0u);
    disk.Savepoint();
    disk.AllocatePage();
    disk.Rollback();
    EXPECT_EQ(disk.SavepointDepth(), 0u);
    EXPECT_EQ(disk.Log().Marks(), 0u);
    EXPECT_EQ(disk.NumPages(), 0);
}

TEST(DiskManager, BeforeWriteHookSeesPagesAndTheCommitRecord) {
    TempFile f;
    DiskManager disk(f.path());
    std::vector<page_id_t> seen;
    disk.SetBeforeWriteHook([&](page_id_t pid) { seen.push_back(pid); });
    char buf[PAGE_SIZE] = {};
    disk.AllocatePage();
    disk.AllocatePage();
    disk.WritePage(1, buf);
    disk.WritePage(0, buf);
    disk.Commit();
    disk.Commit();  // nothing to commit: no record
    EXPECT_EQ(seen, (std::vector<page_id_t>{1, 0, INVALID_PAGE_ID}));
    disk.SetBeforeWriteHook([](page_id_t) { throw DbError("disk gone"); });
    EXPECT_THROW(disk.WritePage(0, buf), DbError);
    EXPECT_EQ(disk.Log().UncommittedPageRecords(), 0u) << "a failed write reaches nothing";
    disk.SetBeforeWriteHook(nullptr);
}

}  // namespace
}  // namespace minidb
