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
    disk.AllocatePage();  // allocation writes a zero page
    EXPECT_EQ(disk.Writes(), 1u);
    char buf[PAGE_SIZE];
    disk.ReadPage(0, buf);
    disk.ReadPage(0, buf);
    EXPECT_EQ(disk.Reads(), 2u);
}

TEST(DiskManager, TruncateDropsTrailingPages) {
    TempFile f;
    {
        DiskManager disk(f.path());
        for (int i = 0; i < 5; ++i) disk.AllocatePage();
        disk.Truncate(2);
        EXPECT_EQ(disk.NumPages(), 2);
        char buf[PAGE_SIZE] = {};
        EXPECT_THROW(disk.ReadPage(2, buf), DbError);
        EXPECT_EQ(disk.AllocatePage(), 2) << "the file grows again from the new end";
        EXPECT_THROW(disk.Truncate(4), DbError) << "cannot 'truncate' to a larger size";
        disk.Sync();
    }
    EXPECT_EQ(std::filesystem::file_size(f.path()), 3 * PAGE_SIZE);
    DiskManager disk(f.path());
    EXPECT_EQ(disk.NumPages(), 3);
}

// Records what the DiskManager tells its listener, interleaved with the actual
// writes (seen through the before-write hook).
struct RecordingListener : DiskManager::WriteListener {
    std::vector<std::string> events;
    void BeforeWrite(page_id_t pid, bool durable) override {
        events.push_back((durable ? "write " : "prepare ") + std::to_string(pid));
    }
    void BeforeGrow() override { events.push_back("grow"); }
};

TEST(DiskManager, ListenerHearsAboutEveryChangeBeforeItHappens) {
    TempFile f;
    DiskManager disk(f.path());
    RecordingListener l;
    disk.SetListener(&l);
    disk.SetBeforeWriteHook([&](page_id_t pid) { l.events.push_back("disk " + std::to_string(pid)); });
    disk.AllocatePage();
    disk.AllocatePage();
    char buf[PAGE_SIZE] = {};
    disk.PrepareWrite(1);
    disk.WritePage(1, buf);
    disk.PrepareWrite(9);  // not a page of the file: ignored
    disk.ReadPage(0, buf);  // reads are not reported
    EXPECT_EQ(l.events, (std::vector<std::string>{"grow", "disk 0", "grow", "disk 1", "prepare 1", "write 1", "disk 1"}));
    disk.SetListener(nullptr);
    disk.WritePage(0, buf);
    EXPECT_EQ(l.events.back(), "disk 0") << "no listener anymore, only the hook";
}

}  // namespace
}  // namespace minidb
