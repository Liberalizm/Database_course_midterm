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

}  // namespace
}  // namespace minidb
