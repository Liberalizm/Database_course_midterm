#include "disk_manager.h"

namespace minidb {

DiskManager::DiskManager(const std::string& path) : path_(path), file_(path, /*create=*/true) {
    num_pages_ = static_cast<page_id_t>(file_.Size() / PAGE_SIZE);
}

void DiskManager::ReadPage(page_id_t pid, char* out) {
    if (pid < 0 || pid >= num_pages_) throw DbError("read of invalid page " + std::to_string(pid));
    size_t got = file_.ReadAt(static_cast<uint64_t>(pid) * PAGE_SIZE, out, PAGE_SIZE);
    if (got < PAGE_SIZE) std::memset(out + got, 0, PAGE_SIZE - got);
    ++reads_;
}

void DiskManager::WritePage(page_id_t pid, const char* data) {
    if (pid < 0 || pid >= num_pages_) throw DbError("write of invalid page " + std::to_string(pid));
    if (listener_) listener_->BeforeWrite(pid, /*durable=*/true);
    RawWrite(pid, data);
}

void DiskManager::RawWrite(page_id_t pid, const char* data) {
    if (before_write_hook_) before_write_hook_(pid);
    file_.WriteAt(static_cast<uint64_t>(pid) * PAGE_SIZE, data, PAGE_SIZE);
    ++writes_;
}

page_id_t DiskManager::AllocatePage() {
    if (listener_) listener_->BeforeGrow();
    char zeros[PAGE_SIZE] = {};
    RawWrite(num_pages_, zeros);  // a new page has no old contents to save
    return num_pages_++;
}

void DiskManager::PrepareWrite(page_id_t pid) {
    if (listener_ && pid >= 0 && pid < num_pages_) listener_->BeforeWrite(pid, /*durable=*/false);
}

void DiskManager::Truncate(page_id_t num_pages) {
    if (num_pages < 0 || num_pages > num_pages_)
        throw DbError("internal: cannot truncate the file to " + std::to_string(num_pages) + " pages");
    if (num_pages == num_pages_) return;
    file_.Truncate(static_cast<uint64_t>(num_pages) * PAGE_SIZE);
    num_pages_ = num_pages;
}

void DiskManager::Sync() { file_.Sync(); }

}  // namespace minidb
