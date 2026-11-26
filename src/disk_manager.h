// disk_manager.h - the lowest layer: reads and writes fixed-size pages, through
// a write-ahead log, so that every commit is atomic and durable.
#pragma once

#include <memory>
#include <string>

#include "common.h"
#include "file_io.h"
#include "wal.h"

namespace minidb {

struct DiskOptions {
    // fsync the log at every commit (and the database file at every checkpoint).
    // Turning it off keeps commits atomic after a process crash, but not after a
    // power failure or OS crash.
    bool sync = true;
    // Checkpoint automatically after a commit once the log holds this many page
    // records (1024 pages = 4 MB). 0: only on Checkpoint() and on clean shutdown.
    size_t checkpoint_pages = 1024;
};

// The database is a single file, viewed as an array of PAGE_SIZE blocks; page N
// lives at byte offset N * PAGE_SIZE. Next to it lives the write-ahead log
// "<file>.wal" (see wal.h).
//
// WritePage does not touch the database file: it appends the page to the log.
// ReadPage returns the newest version, from the log if the page is there.
// Commit() makes all writes since the previous commit durable, as one unit;
// Rollback() forgets them. Checkpoint() copies the committed pages into the
// database file. Opening a database first recovers the log, so the file always
// reflects exactly the last commit, whatever happened before.
class DiskManager {
public:
    explicit DiskManager(const std::string& path, DiskOptions options = {});
    // Clean shutdown: commits, checkpoints and deletes the (now empty) log.
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    void ReadPage(page_id_t pid, char* out);
    void WritePage(page_id_t pid, const char* data);

    // Adds one page to the database and returns its id. Its contents are zeros
    // until it is written; nothing reaches the disk before then.
    page_id_t AllocatePage();

    void Commit();
    void Rollback();
    // Copies every committed page from the log into the database file, fsyncs
    // it, and empties the log. Must not be called with uncommitted writes.
    void Checkpoint();
    bool HasUncommittedChanges() const { return wal_->HasUncommitted() || num_pages_ != committed_num_pages_; }

    page_id_t NumPages() const { return num_pages_; }
    const std::string& Path() const { return path_; }
    const WriteAheadLog& Log() const { return *wal_; }
    static std::string LogPathFor(const std::string& db_path) { return db_path + ".wal"; }

    uint64_t Reads() const { return reads_; }
    uint64_t Writes() const { return writes_; }
    uint64_t Commits() const { return commits_; }
    uint64_t Checkpoints() const { return checkpoints_; }
    // Commits found in the log when the database was opened (i.e. after a crash).
    uint64_t RecoveredCommits() const { return wal_->RecoveredCommits(); }

    // Testing hook: after `n` more successful writes, every WritePage throws,
    // as if the disk had failed. Negative (the default): never.
    void FailWritesAfter(int64_t n) { fail_writes_after_ = n; }

private:
    std::string path_;
    DiskOptions options_;
    File file_;
    std::unique_ptr<WriteAheadLog> wal_;
    page_id_t num_pages_ = 0;
    page_id_t committed_num_pages_ = 0;
    uint64_t reads_ = 0;
    uint64_t writes_ = 0;
    uint64_t commits_ = 0;
    uint64_t checkpoints_ = 0;
    int64_t fail_writes_after_ = -1;
};

}  // namespace minidb
