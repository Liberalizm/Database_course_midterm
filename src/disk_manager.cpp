#include "disk_manager.h"

#include <algorithm>

namespace minidb {

DiskManager::DiskManager(const std::string& path, DiskOptions options) : path_(path), options_(options) {
    if (file_.Open(path) && options_.sync) SyncParentDirectory(path);
    // Recovery: the log keeps every complete commit and drops the rest.
    wal_ = std::make_unique<WriteAheadLog>(LogPathFor(path), options_.sync);
    auto file_pages = static_cast<page_id_t>(file_.Size() / PAGE_SIZE);
    num_pages_ = std::max(file_pages, wal_->HasCommitted() ? wal_->CommittedNumPages() : 0);
    committed_num_pages_ = num_pages_;
    // Apply recovered commits right away, so the database file is up to date.
    if (wal_->HasCommitted()) Checkpoint();
}

DiskManager::~DiskManager() {
    try {
        Commit();
        Checkpoint();
        wal_->CloseAndRemove();
    } catch (...) {
        // never throw from a destructor; the log is kept and recovered on the next open
    }
}

void DiskManager::ReadPage(page_id_t pid, char* out) {
    if (pid < 0 || pid >= num_pages_) throw DbError("read of invalid page " + std::to_string(pid));
    ++reads_;
    if (wal_->Find(pid, out)) return;
    size_t got = file_.ReadAt(static_cast<uint64_t>(pid) * PAGE_SIZE, out, PAGE_SIZE);
    if (got < PAGE_SIZE) std::memset(out + got, 0, PAGE_SIZE - got);  // allocated, never written
}

void DiskManager::WritePage(page_id_t pid, const char* data) {
    if (pid < 0 || pid >= num_pages_) throw DbError("write of invalid page " + std::to_string(pid));
    if (fail_writes_after_ >= 0 && fail_writes_after_-- == 0) {
        fail_writes_after_ = 0;  // stays failed until the test resets it
        throw DbError("I/O error while writing page " + std::to_string(pid) + " (injected)");
    }
    wal_->AppendPage(pid, data);
    ++writes_;
}

page_id_t DiskManager::AllocatePage() { return num_pages_++; }

void DiskManager::Commit() {
    if (!HasUncommittedChanges()) return;
    wal_->Commit(num_pages_);
    committed_num_pages_ = num_pages_;
    ++commits_;
    if (options_.checkpoint_pages > 0 && wal_->PageRecords() >= options_.checkpoint_pages) Checkpoint();
}

void DiskManager::Rollback() {
    wal_->Rollback();
    num_pages_ = committed_num_pages_;
}

void DiskManager::Checkpoint() {
    if (HasUncommittedChanges()) throw DbError("internal: checkpoint with uncommitted changes");
    if (!wal_->HasCommitted()) return;
    wal_->ForEachCommittedPage([this](page_id_t pid, const char* data) {
        file_.WriteAt(static_cast<uint64_t>(pid) * PAGE_SIZE, data, PAGE_SIZE);
    });
    // Allocated pages that were never written still belong to the file (as zeros).
    file_.Truncate(static_cast<uint64_t>(num_pages_) * PAGE_SIZE);
    if (options_.sync) file_.Sync();  // the database file must be durable before the log is emptied
    wal_->Reset();
    ++checkpoints_;
}

}  // namespace minidb
