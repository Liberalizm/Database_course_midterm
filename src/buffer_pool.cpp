#include "buffer_pool.h"

namespace minidb {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk)
    : frames_(pool_size), disk_(disk) {
    if (pool_size < 8) throw DbError("buffer pool must have at least 8 frames");
    for (size_t i = 0; i < pool_size; ++i) free_list_.push_back(static_cast<frame_id_t>(i));
}

BufferPoolManager::~BufferPoolManager() {
    try {
        FlushAll();
    } catch (...) {
        // never throw from a destructor
    }
}

bool BufferPoolManager::GetFreeFrame(frame_id_t* f) {
    if (!free_list_.empty()) {
        *f = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    if (!replacer_.Victim(f)) return false;  // every frame is pinned
    Page& victim = frames_[*f];
    if (victim.dirty_) disk_->WritePage(victim.id_, victim.data_);  // write-back
    page_table_.erase(victim.id_);
    ++evictions_;
    return true;
}

Page* BufferPoolManager::FetchPage(page_id_t pid) {
    auto it = page_table_.find(pid);
    if (it != page_table_.end()) {
        Page& p = frames_[it->second];
        if (p.pin_count_++ == 0) replacer_.Pin(it->second);
        ++hits_;
        return &p;
    }
    ++misses_;
    frame_id_t f;
    if (!GetFreeFrame(&f)) throw DbError("buffer pool exhausted: all frames are pinned");
    Page& p = frames_[f];
    p.Reset();
    disk_->ReadPage(pid, p.data_);
    p.id_ = pid;
    p.pin_count_ = 1;
    page_table_[pid] = f;
    return &p;
}

Page* BufferPoolManager::NewPage(page_id_t* pid) {
    frame_id_t f;
    if (!GetFreeFrame(&f)) throw DbError("buffer pool exhausted: all frames are pinned");
    *pid = disk_->AllocatePage();
    Page& p = frames_[f];
    p.Reset();
    p.id_ = *pid;
    p.pin_count_ = 1;
    p.dirty_ = true;
    page_table_[*pid] = f;
    return &p;
}

bool BufferPoolManager::UnpinPage(page_id_t pid, bool is_dirty) {
    auto it = page_table_.find(pid);
    if (it == page_table_.end()) return false;
    Page& p = frames_[it->second];
    if (p.pin_count_ <= 0) return false;
    if (is_dirty) p.dirty_ = true;
    if (--p.pin_count_ == 0) replacer_.Unpin(it->second);
    return true;
}

void BufferPoolManager::FlushPage(page_id_t pid) {
    auto it = page_table_.find(pid);
    if (it == page_table_.end()) return;
    Page& p = frames_[it->second];
    if (p.dirty_) {
        disk_->WritePage(p.id_, p.data_);
        p.dirty_ = false;
    }
}

void BufferPoolManager::FlushAll() {
    for (auto& [pid, f] : page_table_) {
        Page& p = frames_[f];
        if (p.dirty_) {
            disk_->WritePage(p.id_, p.data_);
            p.dirty_ = false;
        }
    }
}

size_t BufferPoolManager::PinnedFrames() const {
    size_t n = 0;
    for (const auto& p : frames_)
        if (p.pin_count_ > 0) ++n;
    return n;
}

}  // namespace minidb
