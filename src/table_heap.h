// table_heap.h - a table stored as a linked list of slotted pages (heap file).
#pragma once

#include <set>
#include <string>
#include <vector>

#include "buffer_pool.h"
#include "table_page.h"

namespace minidb {

class TableHeap {
public:
    TableHeap(BufferPoolManager* bpm, page_id_t first_page);

    // Allocates and initializes the first page of a new, empty heap.
    static page_id_t Create(BufferPoolManager* bpm);

    RID Insert(const std::string& tuple);
    bool Get(RID rid, std::string* out);
    bool Delete(RID rid);
    // Returns the (possibly new) RID of the updated tuple.
    RID Update(RID rid, const std::string& tuple);

    page_id_t FirstPageId() const { return first_; }
    size_t PageCount() const { return page_count_; }
    // Every page of the heap, in chain order (used to free them on DROP TABLE).
    std::vector<page_id_t> Pages() const;

    // Sequential scan over all live tuples, page by page.
    class Iterator {
    public:
        Iterator(BufferPoolManager* bpm, page_id_t first) : bpm_(bpm), page_(first) {}
        bool Next(RID* rid, std::string* data);

    private:
        BufferPoolManager* bpm_;
        page_id_t page_;
        uint16_t slot_ = 0;
        size_t hops_ = 0;  // pages visited; more than the file has = a cycle
    };
    Iterator Begin() const { return Iterator(bpm_, first_); }

private:
    BufferPoolManager* bpm_;
    page_id_t first_;
    page_id_t last_;
    size_t page_count_ = 0;
    // Tiny in-memory "free space map": pages that recently had tuples deleted.
    std::set<page_id_t> free_hint_;
};

}  // namespace minidb
