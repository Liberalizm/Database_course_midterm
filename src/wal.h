// wal.h - the write-ahead log: full page images, grouped into atomic commits.
//
// Pages are never written to the database file directly. Every page write is
// appended to the log instead, and a commit appends a COMMIT record and fsyncs
// the log. Only then is the statement durable. Later, a checkpoint copies the
// latest committed image of every logged page into the database file, fsyncs
// it, and empties the log.
//
// After a crash, recovery replays the committed part of the log: the pages of
// every complete commit group are applied, and everything after the last
// COMMIT record (an unfinished statement, or a record torn by the crash) is
// ignored. Because records are whole page images, replaying is idempotent and
// also repairs pages that a crash tore in the middle of a checkpoint.
//
// File layout:
//   header  (32 bytes): magic "MDBWAL01", version, page size, salt, the database
//           file's page count when the log was started, checksum
//   records, each with a 32-byte header:
//     type (PAGE / COMMIT), page id, salt, transaction number,
//     page count of the database and digest of the group (COMMIT only), checksum
//   followed, for PAGE records only, by the PAGE_SIZE bytes of the page.
//
// A record is accepted only if its checksum matches and it carries the log's
// salt (which changes every time the log is emptied, so records left over from
// an older generation are never mistaken for new ones). Every record of a
// commit group carries the same transaction number, and numbers are never
// reused (not even after a rollback), so a stale record from a rolled-back
// statement that happens to sit where a newer record should be (a lost write)
// breaks the group instead of being applied. Within a group a page is logged
// once (a second write overwrites the first in place), and the COMMIT record
// carries a digest of the group's records, which exposes an overwrite that was
// lost although the COMMIT record itself reached the disk.
#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "common.h"
#include "file_io.h"

namespace minidb {

class WriteAheadLog {
public:
    static constexpr size_t kHeaderSize = 32;
    static constexpr size_t kRecordHeaderSize = 32;
    static constexpr size_t kPageRecordSize = kRecordHeaderSize + PAGE_SIZE;

    // Opens the log at `path` (creating it if needed) and recovers it: the
    // committed records are indexed, an uncommitted or damaged tail is cut off.
    // With `sync` false, nothing is fsynced (faster, but not crash-safe).
    // `base_pages` is the number of pages in the database file; a new log
    // records it (see BasePages).
    WriteAheadLog(const std::string& path, bool sync, page_id_t base_pages = 0);

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    // Copies the newest logged image of `pid` (committed or not) into `out`.
    // Returns false if the page is not in the log.
    bool Find(page_id_t pid, char* out);

    // Adds a page image to the current (uncommitted) group. A page already in
    // the group is overwritten in place, so a group holds each page once.
    void AppendPage(page_id_t pid, const char* data);
    // Ends the current group with a COMMIT record (recording that the database
    // has `num_pages` pages) and fsyncs the log. Afterwards the group survives a crash.
    void Commit(page_id_t num_pages);
    // Forgets the current group: its records are cut off the log.
    void Rollback();

    bool HasUncommitted() const { return !pending_.empty(); }
    // True if the log holds at least one commit (since it was last emptied).
    bool HasCommitted() const { return has_commit_; }
    // Page count of the database as of the last commit (valid if HasCommitted()).
    page_id_t CommittedNumPages() const { return committed_num_pages_; }
    // Calls fn(pid, image) for the newest committed image of each logged page.
    void ForEachCommittedPage(const std::function<void(page_id_t, const char*)>& fn);
    // Empties the log (after a checkpoint has made its contents redundant).
    // `base_pages`: the number of pages the database file now has.
    void Reset(page_id_t base_pages = 0);
    // Pages the database file had when this log was started (emptied). Pages
    // past that may be missing from the file after a crash in a checkpoint.
    page_id_t BasePages() const { return base_pages_; }
    // Closes the file and deletes it (clean shutdown with an empty log).
    void CloseAndRemove();

    const std::string& Path() const { return path_; }
    uint64_t SizeBytes() const { return end_; }
    size_t PageRecords() const { return page_records_; }  // since the log was last emptied
    uint64_t RecoveredCommits() const { return recovered_commits_; }

private:
    enum RecordType : uint32_t { kPage = 1, kCommit = 2 };

    void Recover();
    void WriteHeader(uint32_t salt);
    // Writes one record at `offset` and returns its checksum.
    uint64_t WriteRecord(uint64_t offset, RecordType type, page_id_t pid, page_id_t num_pages, uint32_t digest,
                         const char* page);
    void Sync();

    struct PendingPage {
        uint64_t offset;  // of the image
        uint64_t mix;     // the record's contribution to the group digest
    };

    std::string path_;
    File file_;
    bool sync_;
    uint32_t salt_ = 0;
    uint32_t txn_ = 1;  // transaction number of the current group
    page_id_t base_pages_ = 0;  // stored in the header

    std::unordered_map<page_id_t, uint64_t> committed_;  // page -> offset of its newest committed image
    std::unordered_map<page_id_t, PendingPage> pending_;  // pages of the current group
    uint64_t pending_sum_ = 0;                            // digest of the current group, before folding
    bool has_commit_ = false;
    page_id_t committed_num_pages_ = 0;
    uint64_t committed_end_ = kHeaderSize;  // end of the last commit group
    size_t committed_page_records_ = 0;
    uint64_t end_ = kHeaderSize;            // end of the log
    size_t page_records_ = 0;
    uint64_t recovered_commits_ = 0;
};

}  // namespace minidb
