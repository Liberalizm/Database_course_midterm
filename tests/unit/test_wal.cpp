// Unit tests: WriteAheadLog (record format, commit groups, recovery).
#include <set>

#include "wal.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::ReadFileBytes;
using testing::TempFile;
using testing::WriteFileBytes;

constexpr size_t kHdr = WriteAheadLog::kHeaderSize;
constexpr size_t kPageRec = WriteAheadLog::kPageRecordSize;
constexpr size_t kCommitRec = WriteAheadLog::kRecordHeaderSize;

std::vector<char> Page(char c) { return std::vector<char>(PAGE_SIZE, c); }

// The first byte of the newest image of `pid` in the log, or 0 if it is not there.
char Lookup(WriteAheadLog& wal, page_id_t pid) {
    std::vector<char> buf(PAGE_SIZE);
    return wal.Find(pid, buf.data()) ? buf[0] : 0;
}

TEST(Wal, NewLogIsEmpty) {
    TempFile f;
    WriteAheadLog wal(f.path(), true);
    EXPECT_FALSE(wal.HasCommitted());
    EXPECT_FALSE(wal.HasUncommitted());
    EXPECT_EQ(wal.SizeBytes(), kHdr);
    EXPECT_EQ(std::filesystem::file_size(f.path()), kHdr);
    EXPECT_EQ(Lookup(wal, 0), 0);
}

TEST(Wal, FindReturnsTheNewestImage) {
    TempFile f;
    WriteAheadLog wal(f.path(), false);
    wal.AppendPage(3, Page('a').data());
    wal.Commit(4);
    EXPECT_EQ(Lookup(wal, 3), 'a');
    wal.AppendPage(3, Page('b').data());
    EXPECT_EQ(Lookup(wal, 3), 'b') << "uncommitted images are visible to the writer";
    wal.Rollback();
    EXPECT_EQ(Lookup(wal, 3), 'a');
    EXPECT_EQ(wal.SizeBytes(), kHdr + kPageRec + kCommitRec) << "rollback cuts the group off";
    EXPECT_EQ(std::filesystem::file_size(f.path()), wal.SizeBytes());
}

TEST(Wal, PageWrittenTwiceInAGroupIsLoggedOnce) {
    TempFile f;
    {
        WriteAheadLog wal(f.path(), false);
        wal.AppendPage(1, Page('a').data());
        wal.Commit(2);
        wal.AppendPage(1, Page('b').data());  // a new group: may not touch the committed 'a'
        wal.AppendPage(0, Page('x').data());
        wal.AppendPage(1, Page('c').data());  // same group: overwrites 'b'
        EXPECT_EQ(wal.PageRecords(), 3u);
        EXPECT_EQ(wal.SizeBytes(), kHdr + 3 * kPageRec + kCommitRec);
        EXPECT_EQ(Lookup(wal, 1), 'c');
        wal.Commit(2);
    }
    WriteAheadLog wal(f.path(), false);
    EXPECT_EQ(wal.RecoveredCommits(), 2u);
    EXPECT_EQ(Lookup(wal, 1), 'c');
    EXPECT_EQ(Lookup(wal, 0), 'x');
}

// The power fails while the commit is being synced: the COMMIT record reached
// the disk, but the in-place overwrite of an earlier record of the group did
// not. That record is intact and from the right transaction, but the group's
// digest no longer matches, so the group is not applied.
TEST(Wal, LostOverwriteWithinACommittedGroupIsDetected) {
    TempFile f;
    std::vector<char> first_version;
    const size_t group_start = kHdr + kPageRec + kCommitRec;
    {
        WriteAheadLog wal(f.path(), false);
        wal.AppendPage(0, Page('a').data());
        wal.Commit(2);
        wal.AppendPage(1, Page('b').data());
        first_version = ReadFileBytes(f.path());
        wal.AppendPage(1, Page('c').data());  // overwrites 'b' in place
        wal.Commit(2);
    }
    std::vector<char> bytes = ReadFileBytes(f.path());
    std::copy(first_version.begin() + static_cast<std::ptrdiff_t>(group_start), first_version.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(group_start));
    WriteFileBytes(f.path(), bytes);
    WriteAheadLog wal(f.path(), false);
    EXPECT_EQ(wal.RecoveredCommits(), 1u);
    EXPECT_EQ(Lookup(wal, 1), 0) << "the stale 'b' image must not be applied";
}

TEST(Wal, RecoveryKeepsCommittedGroupsAndDropsTheRest) {
    TempFile f;
    {
        WriteAheadLog wal(f.path(), true);
        wal.AppendPage(1, Page('a').data());
        wal.AppendPage(2, Page('b').data());
        wal.Commit(5);
        wal.AppendPage(1, Page('c').data());
        wal.Commit(6);
        wal.AppendPage(2, Page('z').data());  // never committed: the "crash" comes first
    }
    WriteAheadLog wal(f.path(), true);
    EXPECT_TRUE(wal.HasCommitted());
    EXPECT_EQ(wal.RecoveredCommits(), 2u);
    EXPECT_EQ(wal.CommittedNumPages(), 6);
    EXPECT_EQ(Lookup(wal, 1), 'c');
    EXPECT_EQ(Lookup(wal, 2), 'b');
    EXPECT_EQ(wal.PageRecords(), 3u);
    EXPECT_EQ(std::filesystem::file_size(f.path()), kHdr + 3 * kPageRec + 2 * kCommitRec)
        << "the uncommitted tail is cut off";
}

TEST(Wal, ForEachCommittedPageVisitsNewestImagesOnly) {
    TempFile f;
    WriteAheadLog wal(f.path(), false);
    wal.AppendPage(7, Page('a').data());
    wal.AppendPage(2, Page('b').data());
    wal.AppendPage(7, Page('c').data());
    wal.Commit(8);
    wal.AppendPage(9, Page('u').data());  // uncommitted: not visited
    std::vector<std::pair<page_id_t, char>> seen;
    wal.ForEachCommittedPage([&](page_id_t pid, const char* data) { seen.emplace_back(pid, data[0]); });
    std::vector<std::pair<page_id_t, char>> expected = {{2, 'b'}, {7, 'c'}};
    EXPECT_EQ(seen, expected);
}

// A crash can cut the log at any byte. Whatever the cut, recovery must give
// exactly the commits that were complete before it.
TEST(Wal, TornTailAtEveryOffset) {
    TempFile f;
    {
        WriteAheadLog wal(f.path(), false);
        wal.AppendPage(0, Page('a').data());
        wal.Commit(1);
        wal.AppendPage(0, Page('b').data());
        wal.AppendPage(1, Page('c').data());
        wal.Commit(2);
    }
    const std::vector<char> full = ReadFileBytes(f.path());
    const size_t first_commit_end = kHdr + kPageRec + kCommitRec;
    ASSERT_EQ(full.size(), first_commit_end + 2 * kPageRec + kCommitRec);
    // Every 61st byte, and every byte near a record boundary.
    std::set<size_t> cuts;
    for (size_t len = 0; len <= full.size(); len += 61) cuts.insert(len);
    size_t boundaries[] = {0, kHdr, kHdr + kPageRec, first_commit_end, first_commit_end + kPageRec,
                           first_commit_end + 2 * kPageRec, full.size()};
    for (size_t b : boundaries)
        for (size_t d = 0; d <= 4; ++d) {
            if (b >= d) cuts.insert(b - d);
            if (b + d <= full.size()) cuts.insert(b + d);
        }
    std::string cut = f.path() + ".cut";
    for (size_t len : cuts) {
        WriteFileBytes(cut, full.data(), len);
        WriteAheadLog wal(cut, false);
        if (len == full.size()) {
            EXPECT_EQ(wal.RecoveredCommits(), 2u);
            EXPECT_EQ(Lookup(wal, 0), 'b');
            EXPECT_EQ(Lookup(wal, 1), 'c');
        } else if (len >= first_commit_end) {
            EXPECT_EQ(wal.RecoveredCommits(), 1u) << "cut at " << len;
            EXPECT_EQ(Lookup(wal, 0), 'a') << "cut at " << len;
            EXPECT_EQ(Lookup(wal, 1), 0) << "cut at " << len;
        } else {
            EXPECT_FALSE(wal.HasCommitted()) << "cut at " << len;
        }
    }
    std::remove(cut.c_str());
}

TEST(Wal, DamagedRecordEndsRecovery) {
    TempFile f;
    {
        WriteAheadLog wal(f.path(), false);
        wal.AppendPage(0, Page('a').data());
        wal.Commit(1);
        wal.AppendPage(0, Page('b').data());
        wal.Commit(1);
        wal.AppendPage(0, Page('c').data());
        wal.Commit(1);
    }
    std::vector<char> bytes = ReadFileBytes(f.path());
    size_t second_group = kHdr + kPageRec + kCommitRec;
    bytes[second_group + WriteAheadLog::kRecordHeaderSize + 1000] ^= 0x01;  // one bit of the second image
    WriteFileBytes(f.path(), bytes);
    WriteAheadLog wal(f.path(), false);
    EXPECT_EQ(wal.RecoveredCommits(), 1u) << "nothing after the damaged record can be trusted";
    EXPECT_EQ(Lookup(wal, 0), 'a');
}

// A lost write: the disk still holds a record of a rolled-back statement where a
// newer record should be. Its checksum is valid, but its transaction number
// gives it away, so the newer commit is not accepted with a stale page in it.
TEST(Wal, StaleRecordFromARolledBackGroupIsNotApplied) {
    TempFile f;
    std::vector<char> stale;
    {
        WriteAheadLog wal(f.path(), false);
        wal.AppendPage(0, Page('a').data());
        wal.Commit(1);
        size_t group_start = wal.SizeBytes();
        wal.AppendPage(0, Page('X').data());
        stale = ReadFileBytes(f.path());
        stale.erase(stale.begin(), stale.begin() + static_cast<std::ptrdiff_t>(group_start));
        wal.Rollback();
        wal.AppendPage(0, Page('b').data());
        wal.Commit(1);
    }
    std::vector<char> bytes = ReadFileBytes(f.path());
    size_t group_start = kHdr + kPageRec + kCommitRec;
    ASSERT_EQ(stale.size(), kPageRec);
    std::copy(stale.begin(), stale.end(), bytes.begin() + static_cast<std::ptrdiff_t>(group_start));
    WriteFileBytes(f.path(), bytes);
    WriteAheadLog wal(f.path(), false);
    EXPECT_EQ(wal.RecoveredCommits(), 1u);
    EXPECT_EQ(Lookup(wal, 0), 'a') << "the stale 'X' image must never be applied";
}

// Records left over from before the log was emptied must not be read as new.
TEST(Wal, ResetStartsANewGeneration) {
    TempFile f;
    std::vector<char> old_records;
    {
        WriteAheadLog wal(f.path(), false);
        wal.AppendPage(0, Page('o').data());
        wal.Commit(1);
        old_records = ReadFileBytes(f.path());
        old_records.erase(old_records.begin(), old_records.begin() + kHdr);
        wal.Reset();
        EXPECT_FALSE(wal.HasCommitted());
        EXPECT_EQ(Lookup(wal, 0), 0);
    }
    std::vector<char> bytes = ReadFileBytes(f.path());
    ASSERT_EQ(bytes.size(), kHdr);
    bytes.insert(bytes.end(), old_records.begin(), old_records.end());  // as if truncation was lost
    WriteFileBytes(f.path(), bytes);
    WriteAheadLog wal(f.path(), false);
    EXPECT_FALSE(wal.HasCommitted());
    EXPECT_EQ(Lookup(wal, 0), 0);
}

TEST(Wal, GarbageFileIsTreatedAsAnEmptyLog) {
    TempFile f;
    WriteFileBytes(f.path(), std::vector<char>(10000, 'J'));
    WriteAheadLog wal(f.path(), false);
    EXPECT_FALSE(wal.HasCommitted());
    EXPECT_EQ(std::filesystem::file_size(f.path()), kHdr);
    wal.AppendPage(0, Page('n').data());
    wal.Commit(1);
    EXPECT_EQ(Lookup(wal, 0), 'n');
}

TEST(Wal, CommitWithoutPagesRecordsTheSize) {
    TempFile f;
    {
        WriteAheadLog wal(f.path(), true);
        wal.Commit(42);
    }
    WriteAheadLog wal(f.path(), true);
    EXPECT_TRUE(wal.HasCommitted());
    EXPECT_EQ(wal.CommittedNumPages(), 42);
}

TEST(Wal, CannotResetDuringAGroup) {
    TempFile f;
    WriteAheadLog wal(f.path(), false);
    wal.AppendPage(0, Page('a').data());
    EXPECT_THROW(wal.Reset(), DbError);
    wal.Commit(1);
    EXPECT_NO_THROW(wal.Reset());
}

TEST(Wal, CloseAndRemoveDeletesTheFile) {
    TempFile f;
    WriteAheadLog wal(f.path(), false);
    wal.CloseAndRemove();
    EXPECT_FALSE(std::filesystem::exists(f.path()));
}

// ---- marks (savepoints inside a group) ----

TEST(Wal, RollbackToMarkRestoresTheImagesAsOfTheMark) {
    TempFile f;
    WriteAheadLog wal(f.path(), false);
    wal.AppendPage(0, Page('a').data());
    wal.Commit(3);
    wal.AppendPage(1, Page('b').data());
    const uint64_t at_mark = wal.SizeBytes();
    wal.SetMark();
    EXPECT_EQ(wal.Marks(), 1u);
    wal.AppendPage(1, Page('c').data());  // written before the mark: appended, not overwritten
    wal.AppendPage(0, Page('d').data());
    wal.AppendPage(2, Page('e').data());
    wal.AppendPage(2, Page('f').data());  // written after the mark: overwritten in place
    EXPECT_EQ(wal.SizeBytes(), at_mark + 3 * kPageRec);
    EXPECT_EQ(Lookup(wal, 1), 'c');

    wal.RollbackToMark();
    EXPECT_EQ(wal.Marks(), 1u) << "the mark stays";
    EXPECT_EQ(wal.SizeBytes(), at_mark);
    EXPECT_EQ(std::filesystem::file_size(f.path()), at_mark);
    EXPECT_EQ(Lookup(wal, 1), 'b');
    EXPECT_EQ(Lookup(wal, 0), 'a') << "back to the committed image";
    EXPECT_EQ(Lookup(wal, 2), 0);
    EXPECT_TRUE(wal.HasUncommitted());

    wal.AppendPage(2, Page('g').data());  // the group goes on from the mark
    wal.ReleaseMark();
    EXPECT_EQ(wal.Marks(), 0u);
    wal.Commit(3);
    EXPECT_EQ(Lookup(wal, 1), 'b');
    EXPECT_EQ(Lookup(wal, 2), 'g');
}

TEST(Wal, NestedMarks) {
    TempFile f;
    WriteAheadLog wal(f.path(), false);
    wal.AppendPage(0, Page('a').data());
    wal.SetMark();
    wal.AppendPage(0, Page('b').data());
    wal.SetMark();
    wal.AppendPage(0, Page('c').data());
    wal.RollbackToMark();
    EXPECT_EQ(Lookup(wal, 0), 'b');
    wal.ReleaseMark();
    wal.AppendPage(0, Page('d').data());  // after the outer mark only: overwrites 'b'
    EXPECT_EQ(Lookup(wal, 0), 'd');
    wal.RollbackToMark();
    EXPECT_EQ(Lookup(wal, 0), 'a');
    wal.ReleaseMark();
    EXPECT_THROW(wal.ReleaseMark(), DbError);
    EXPECT_THROW(wal.RollbackToMark(), DbError);
}

TEST(Wal, CommitAndRollbackDropTheMarks) {
    TempFile f;
    WriteAheadLog wal(f.path(), false);
    wal.SetMark();
    wal.AppendPage(0, Page('a').data());
    wal.SetMark();
    wal.Commit(1);
    EXPECT_EQ(wal.Marks(), 0u);
    wal.SetMark();
    wal.AppendPage(0, Page('b').data());
    wal.Rollback();
    EXPECT_EQ(wal.Marks(), 0u);
    EXPECT_EQ(Lookup(wal, 0), 'a');
}

// A group that went through marks holds some pages twice (the image from before
// a mark, and the newer one appended after it). Recovery must apply the newer.
TEST(Wal, GroupWithPagesLoggedTwiceRecoversTheNewestImages) {
    TempFile f;
    {
        WriteAheadLog wal(f.path(), false);
        wal.AppendPage(0, Page('a').data());
        wal.AppendPage(1, Page('b').data());
        wal.SetMark();
        wal.AppendPage(0, Page('c').data());
        wal.AppendPage(1, Page('x').data());
        wal.RollbackToMark();
        wal.AppendPage(1, Page('d').data());
        wal.ReleaseMark();
        wal.AppendPage(1, Page('e').data());  // no mark left: overwrites 'd' in place
        wal.Commit(2);
        EXPECT_EQ(wal.PageRecords(), 3u);
    }
    WriteAheadLog wal(f.path(), false);
    EXPECT_EQ(wal.RecoveredCommits(), 1u);
    EXPECT_EQ(Lookup(wal, 0), 'a');
    EXPECT_EQ(Lookup(wal, 1), 'e');
}

// The records cut off by RollbackToMark carry the group's transaction number.
// If one of them is found where a newer record should be (that write was lost),
// the COMMIT record's digest must reject the group instead of applying it.
TEST(Wal, StaleRecordCutOffByAMarkIsNotApplied) {
    TempFile f;
    std::vector<char> with_stale;
    const size_t mark_at = kHdr + kPageRec;
    {
        WriteAheadLog wal(f.path(), false);
        wal.AppendPage(0, Page('a').data());
        wal.SetMark();
        wal.AppendPage(1, Page('x').data());  // will be cut off
        with_stale = ReadFileBytes(f.path());
        wal.RollbackToMark();
        wal.AppendPage(1, Page('y').data());  // lands where 'x' was
        wal.Commit(2);
    }
    std::vector<char> bytes = ReadFileBytes(f.path());
    std::copy(with_stale.begin() + static_cast<std::ptrdiff_t>(mark_at), with_stale.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(mark_at));  // the write of 'y' was lost
    WriteFileBytes(f.path(), bytes);
    WriteAheadLog wal(f.path(), false);
    EXPECT_EQ(wal.RecoveredCommits(), 0u);
    EXPECT_EQ(Lookup(wal, 1), 0) << "the cut-off 'x' image must not be applied";
}

TEST(Wal, HeaderRecordsTheBasePageCount) {
    TempFile f;
    {
        WriteAheadLog wal(f.path(), false, 7);
        EXPECT_EQ(wal.BasePages(), 7);
        wal.AppendPage(0, Page('a').data());
        wal.Commit(9);
    }
    {
        WriteAheadLog wal(f.path(), false, 3);  // a log with commits keeps its own
        EXPECT_EQ(wal.BasePages(), 7);
        wal.Reset(9);
        EXPECT_EQ(wal.BasePages(), 9);
    }
    WriteAheadLog wal(f.path(), false, 4);  // an empty log starts over with the given count
    EXPECT_EQ(wal.BasePages(), 4);
}

}  // namespace
}  // namespace minidb
