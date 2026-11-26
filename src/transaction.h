// transaction.h - atomic, crash-safe transactions with a rollback journal.
//
// The idea (the same as SQLite's rollback journal): before a page of the
// database file is overwritten for the first time in a transaction, its
// original contents are appended to "<database>-journal", and the journal is
// synced to disk before the page itself is written.
//
//   COMMIT    sync the database file, then delete the journal. Deleting the
//             journal is the commit point: until it is gone, the transaction
//             can still be undone.
//   ROLLBACK  write every saved page back, shrink the file to its original
//             length (pages added by the transaction disappear), sync, delete
//             the journal.
//   crash     the journal is still there when the database is opened next
//             ("hot journal"), so the rollback simply happens then.
//
// A page is journaled at most once per transaction (its contents as of BEGIN
// are all a rollback needs), and pages that did not exist at BEGIN never are.
//
// Savepoints nest inside a transaction, and the Database also wraps every
// statement of an explicit transaction in one, so a failing statement is undone
// without ending the transaction. A savepoint level only has to survive as long
// as the process does (a crash rolls back the whole transaction), so it keeps
// its page images in memory: when it starts, every dirty page has been flushed,
// so the file holds the state to return to; the old contents of each page
// written to the file after that are captured just before the write.
//
// This class only manages the file. The caller decides when, and keeps the
// buffer pool consistent: everything must be flushed before Begin, Savepoint
// and Commit, and the pool must be emptied before (or right after, without
// writing anything back) Rollback and RollbackToSavepoint, after which the
// catalog has to be reloaded from the restored pages.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "disk_manager.h"
#include "os_file.h"

namespace minidb {

class TransactionManager : public DiskManager::WriteListener {
public:
    // If a hot journal is found, the interrupted transaction is rolled back
    // before anything else happens. `synchronous` = false skips every fsync:
    // still safe if the process crashes, but not if the machine loses power.
    explicit TransactionManager(DiskManager* disk, bool synchronous = true);
    ~TransactionManager() override;

    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    static std::string JournalPath(const std::string& db_path) { return db_path + "-journal"; }

    void Begin();
    void Commit();
    void Rollback();
    bool Active() const { return active_; }

    void Savepoint();
    // Ends the innermost savepoint, keeping its changes (they now belong to the
    // enclosing savepoint or the transaction).
    void ReleaseSavepoint();
    // Undoes everything since the innermost savepoint started. The savepoint
    // itself stays (like SQL's ROLLBACK TO SAVEPOINT).
    void RollbackToSavepoint();
    size_t SavepointDepth() const { return levels_.size(); }

    // True if a hot journal was rolled back when the database was opened.
    bool RecoveredOnOpen() const { return recovered_; }
    // Pages saved in the journal file by the current transaction.
    size_t JournaledPages() const { return journaled_.size(); }

    void BeforeWrite(page_id_t pid, bool durable) override;
    void BeforeGrow() override;

private:
    struct Level {
        page_id_t start_pages = 0;                            // file length when the savepoint began
        std::unordered_map<page_id_t, std::string> images;  // page -> contents when it began
    };

    void Recover();
    void OpenJournal();
    void AppendToJournal(page_id_t pid, const std::string& data);
    void SyncJournal();
    // Writes the journaled pages back and shrinks the file to its original length.
    // Returns false if the journal's header is invalid (then nothing is changed).
    bool ApplyJournal(File& journal);
    void DeleteJournal();
    void Reset();

    DiskManager* disk_;
    bool sync_;
    std::string journal_path_;
    File journal_;
    uint64_t journal_end_ = 0;     // where the next record goes
    bool journal_synced_ = true;   // every record appended so far is durable

    bool active_ = false;
    page_id_t orig_pages_ = 0;              // file length at BEGIN
    std::unordered_set<page_id_t> journaled_;
    std::vector<Level> levels_;             // savepoints, innermost last
    bool restoring_ = false;                // writing old images back: capture nothing
    bool recovered_ = false;
};

}  // namespace minidb
