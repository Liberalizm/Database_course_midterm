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
    disk.AllocatePage();  // allocation writes a zero page
    EXPECT_EQ(disk.Writes(), 1u);
    char buf[PAGE_SIZE];
    disk.ReadPage(0, buf);
    disk.ReadPage(0, buf);
    EXPECT_EQ(disk.Reads(), 2u);
}

}  // namespace
}  // namespace minidb
