#include "disk_manager.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "checksum.h"

namespace minidb {

namespace {

uint64_t PageOffset(page_id_t pid) { return static_cast<uint64_t>(pid) * DISK_PAGE_SIZE; }

std::string Hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(v));
    return buf;
}

}  // namespace

DiskManager::DiskManager(const std::string& path, DiskOptions options) : path_(path), options_(options) {
    if (file_.Open(path) && options_.sync) SyncParentDirectory(path);
    const uint64_t size = file_.Size();
    const bool whole_pages = size % DISK_PAGE_SIZE == 0;
    // Either the file is damaged (truncated, or something else entirely) or it
    // was written by an older MiniDB whose pages had no checksums. The one
    // exception is a crash while a checkpoint was growing the file: then the log
    // still holds every page of it, and the checkpoint below repairs the file.
    auto invalid = [&] {
        return DbError("not a valid database file: " + path + " (size " + std::to_string(size) +
                       " is not a multiple of the " + std::to_string(DISK_PAGE_SIZE) + "-byte page)");
    };
    if (!whole_pages && !std::filesystem::exists(LogPathFor(path))) throw invalid();
    // Recovery: the log keeps every complete commit and drops the rest.
    file_pages_ = static_cast<page_id_t>(size / DISK_PAGE_SIZE);
    wal_ = std::make_unique<WriteAheadLog>(LogPathFor(path), options_.sync, file_pages_);
    if (!whole_pages && !wal_->HasCommitted()) throw invalid();
    num_pages_ = std::max(file_pages_, wal_->HasCommitted() ? wal_->CommittedNumPages() : 0);
    committed_num_pages_ = num_pages_;
    // A crash in a checkpoint may have left holes among the pages it added to
    // the file; the checkpoint below writes every one of them again.
    if (wal_->HasCommitted()) file_pages_ = std::min(file_pages_, wal_->BasePages());
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
    if (pid >= file_pages_) {  // allocated, never written
        std::memset(out, 0, PAGE_SIZE);
        return;
    }
    char frame[DISK_PAGE_SIZE];
    if (file_.ReadAt(PageOffset(pid), frame, DISK_PAGE_SIZE) != DISK_PAGE_SIZE) {
        // The file shrank under us (we checked file_pages_ above).
        throw DbError("I/O error while reading page " + std::to_string(pid) + " (file truncated?)");
    }
    std::memcpy(out, frame, PAGE_SIZE);
    uint32_t expected = ReadAs<uint32_t>(frame + PAGE_SIZE);
    uint32_t actual = PageChecksum(pid, out);
    if (actual != expected) {
        throw DbError("page " + std::to_string(pid) + " failed its checksum (stored " + Hex32(expected) +
                      ", computed " + Hex32(actual) + "): the database file is damaged");
    }
}

void DiskManager::WritePage(page_id_t pid, const char* data) {
    if (pid < 0 || pid >= num_pages_) throw DbError("write of invalid page " + std::to_string(pid));
    if (before_write_hook_) before_write_hook_(pid);
    if (fail_writes_after_ >= 0 && fail_writes_after_-- == 0) {
        fail_writes_after_ = 0;  // stays failed until the test resets it
        throw DbError("I/O error while writing page " + std::to_string(pid) + " (injected)");
    }
    wal_->AppendPage(pid, data);
    ++writes_;
}

page_id_t DiskManager::AllocatePage() { return num_pages_++; }

void DiskManager::Commit() {
    if (HasUncommittedChanges()) {
        if (before_write_hook_) before_write_hook_(INVALID_PAGE_ID);
        wal_->Commit(num_pages_);  // ends the savepoints too
        committed_num_pages_ = num_pages_;
        ++commits_;
    } else {
        while (wal_->Marks() > 0) wal_->ReleaseMark();
    }
    savepoint_pages_.clear();
    if (options_.checkpoint_pages > 0 && wal_->PageRecords() >= options_.checkpoint_pages) {
        // The commit is durable already, so a failing checkpoint must not turn
        // into an error of the commit. The log keeps every page, and the next
        // checkpoint (at the latest on shutdown or reopen) tries again.
        try {
            Checkpoint();
        } catch (const DbError&) {
        }
    }
}

void DiskManager::Rollback() {
    savepoint_pages_.clear();
    num_pages_ = committed_num_pages_;
    wal_->Rollback();
}

void DiskManager::Savepoint() {
    wal_->SetMark();
    savepoint_pages_.push_back(num_pages_);
}

void DiskManager::RollbackToSavepoint() {
    if (savepoint_pages_.empty()) throw DbError("internal: no savepoint to roll back to");
    num_pages_ = savepoint_pages_.back();
    wal_->RollbackToMark();
}

void DiskManager::ReleaseSavepoint() {
    if (savepoint_pages_.empty()) throw DbError("internal: no savepoint to release");
    savepoint_pages_.pop_back();
    wal_->ReleaseMark();
}

void DiskManager::Checkpoint() {
    if (HasUncommittedChanges()) throw DbError("internal: checkpoint with uncommitted changes");
    if (!wal_->HasCommitted()) return;
    // Each page goes to the file together with its checksum.
    std::vector<bool> written(static_cast<size_t>(num_pages_), false);
    char frame[DISK_PAGE_SIZE];
    auto put = [&](page_id_t pid, const char* data) {
        std::memcpy(frame, data, PAGE_SIZE);
        WriteAs<uint32_t>(frame + PAGE_SIZE, PageChecksum(pid, data));
        file_.WriteAt(PageOffset(pid), frame, DISK_PAGE_SIZE);
    };
    wal_->ForEachCommittedPage([&](page_id_t pid, const char* data) {
        if (pid >= num_pages_) return;  // cannot happen: every commit covers the pages it wrote
        put(pid, data);
        written[static_cast<size_t>(pid)] = true;
    });
    // Pages added since the last checkpoint but never written still belong to
    // the file, as zeros (with a valid checksum).
    static const char kZeros[PAGE_SIZE] = {};
    for (page_id_t pid = file_pages_; pid < num_pages_; ++pid)
        if (!written[static_cast<size_t>(pid)]) put(pid, kZeros);
    file_.Truncate(PageOffset(num_pages_));  // also drops a page torn by a crash while the file grew
    if (options_.sync) file_.Sync();          // the database file must be durable before the log is emptied
    file_pages_ = num_pages_;
    wal_->Reset(num_pages_);
    ++checkpoints_;
}

}  // namespace minidb
