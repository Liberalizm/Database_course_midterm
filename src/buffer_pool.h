// buffer_pool.h - in-memory page cache with pinning and LRU replacement.
#pragma once

#include <list>
#include <set>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "disk_manager.h"

namespace minidb {

// A frame of the buffer pool: one page worth of bytes plus bookkeeping.
class Page {
public:
    char* Data() { return data_; }
    const char* Data() const { return data_; }
    page_id_t Id() const { return id_; }
    int PinCount() const { return pin_count_; }
    bool IsDirty() const { return dirty_; }

private:
    friend class BufferPoolManager;
    void Reset() {
        std::memset(data_, 0, PAGE_SIZE);
        id_ = INVALID_PAGE_ID;
        pin_count_ = 0;
        dirty_ = false;
    }

    alignas(8) char data_[PAGE_SIZE] = {};
    page_id_t id_ = INVALID_PAGE_ID;
    int pin_count_ = 0;
    bool dirty_ = false;
};

// Least-Recently-Used replacement policy. Only frames whose pin count is 0
// are tracked here; they are candidates for eviction.
class LRUReplacer {
public:
    // Frame became unpinned -> it is now the most recently used evictable frame.
    void Unpin(frame_id_t f) {
        if (pos_.count(f)) return;
        lru_.push_front(f);
        pos_[f] = lru_.begin();
    }
    // Frame is in use again -> it must not be evicted.
    void Pin(frame_id_t f) {
        auto it = pos_.find(f);
        if (it == pos_.end()) return;
        lru_.erase(it->second);
        pos_.erase(it);
    }
    // Pick the least recently used evictable frame.
    bool Victim(frame_id_t* f) {
        if (lru_.empty()) return false;
        *f = lru_.back();
        lru_.pop_back();
        pos_.erase(*f);
        return true;
    }
    size_t Size() const { return lru_.size(); }

private:
    std::list<frame_id_t> lru_;  // front = most recent, back = least recent
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> pos_;
};

// The buffer pool manager. Every access to a page goes through here:
//   FetchPage -> page is pinned (cannot be evicted) until UnpinPage.
// Dirty pages are written back to disk on eviction or flush.
class BufferPoolManager {
public:
    BufferPoolManager(size_t pool_size, DiskManager* disk);
    ~BufferPoolManager();

    Page* FetchPage(page_id_t pid);
    // Returns a zeroed, pinned page. Reuses a freed page if there is one, and
    // only grows the database file when the free-page list is empty.
    Page* NewPage(page_id_t* pid);
    bool UnpinPage(page_id_t pid, bool is_dirty);
    void FlushPage(page_id_t pid);
    void FlushAll();
    bool HasDirtyPages() const;
    // Empties the pool without writing anything back. Used when a statement is
    // rolled back: any cached page, dirty or not, may hold its changes. No page
    // may be pinned. The catalog restores the free-page list when it reloads.
    void DiscardAll();

    // Gives a page back for reuse (e.g. the pages of a dropped table). The page
    // must not be pinned; its cached contents are discarded without write-back.
    void DeletePage(page_id_t pid);
    // The free-page list is persisted by the catalog, which restores it on startup.
    const std::set<page_id_t>& FreePages() const { return free_pages_; }
    void SetFreePages(std::set<page_id_t> pages) { free_pages_ = std::move(pages); }

    DiskManager* Disk() { return disk_; }
    size_t PoolSize() const { return frames_.size(); }
    size_t PinnedFrames() const;

    uint64_t Hits() const { return hits_; }
    uint64_t Misses() const { return misses_; }
    uint64_t Evictions() const { return evictions_; }
    void ResetStats() { hits_ = misses_ = evictions_ = 0; }

private:
    bool GetFreeFrame(frame_id_t* f);

    std::vector<Page> frames_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;  // which page is in which frame
    std::list<frame_id_t> free_list_;                         // frames holding no page
    std::set<page_id_t> free_pages_;                          // pages on disk available for reuse
    LRUReplacer replacer_;
    DiskManager* disk_;

    uint64_t hits_ = 0, misses_ = 0, evictions_ = 0;
};

// RAII helper: pins a page on construction and unpins it on destruction,
// so a page can never be "leaked" in the pinned state by an early return/exception.
class PageGuard {
public:
    PageGuard() = default;
    PageGuard(BufferPoolManager* bpm, Page* p) : bpm_(bpm), page_(p) {}

    static PageGuard Fetch(BufferPoolManager* bpm, page_id_t pid) {
        return PageGuard(bpm, bpm->FetchPage(pid));
    }
    static PageGuard New(BufferPoolManager* bpm, page_id_t* pid) {
        return PageGuard(bpm, bpm->NewPage(pid));
    }

    PageGuard(PageGuard&& o) noexcept : bpm_(o.bpm_), page_(o.page_), dirty_(o.dirty_) {
        o.page_ = nullptr;
    }
    PageGuard& operator=(PageGuard&& o) noexcept {
        if (this != &o) {
            Release();
            bpm_ = o.bpm_;
            page_ = o.page_;
            dirty_ = o.dirty_;
            o.page_ = nullptr;
        }
        return *this;
    }
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    ~PageGuard() { Release(); }

    char* Data() { return page_->Data(); }
    const char* Data() const { return page_->Data(); }
    page_id_t Id() const { return page_->Id(); }
    void MarkDirty() { dirty_ = true; }

    void Release() {
        if (page_) {
            bpm_->UnpinPage(page_->Id(), dirty_);
            page_ = nullptr;
        }
    }

private:
    BufferPoolManager* bpm_ = nullptr;
    Page* page_ = nullptr;
    bool dirty_ = false;
};

}  // namespace minidb
