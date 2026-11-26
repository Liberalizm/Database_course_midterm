#include "table_heap.h"

namespace minidb {

TableHeap::TableHeap(BufferPoolManager* bpm, page_id_t first_page)
    : bpm_(bpm), first_(first_page), last_(first_page) {
    page_id_t p = first_;
    while (p != INVALID_PAGE_ID) {
        PageGuard g = PageGuard::Fetch(bpm_, p);
        last_ = p;
        ++page_count_;
        p = TablePage(g.Data()).NextPageId();
    }
}

page_id_t TableHeap::Create(BufferPoolManager* bpm) {
    page_id_t pid;
    PageGuard g = PageGuard::New(bpm, &pid);
    TablePage(g.Data()).Init();
    g.MarkDirty();
    return pid;
}

RID TableHeap::Insert(const std::string& tuple) {
    if (tuple.size() > TablePage::MAX_TUPLE)
        throw DbError("row is too large (" + std::to_string(tuple.size()) + " bytes, max " +
                      std::to_string(TablePage::MAX_TUPLE) + ")");

    // 1. pages known to have free space after deletes
    while (!free_hint_.empty()) {
        page_id_t p = *free_hint_.begin();
        PageGuard g = PageGuard::Fetch(bpm_, p);
        auto slot = TablePage(g.Data()).Insert(tuple.data(), tuple.size());
        if (slot) {
            g.MarkDirty();
            return RID{p, *slot};
        }
        free_hint_.erase(free_hint_.begin());
    }
    // 2. the last page of the chain
    {
        PageGuard g = PageGuard::Fetch(bpm_, last_);
        auto slot = TablePage(g.Data()).Insert(tuple.data(), tuple.size());
        if (slot) {
            g.MarkDirty();
            return RID{last_, *slot};
        }
    }
    // 3. extend the heap with a new page
    page_id_t np;
    PageGuard ng = PageGuard::New(bpm_, &np);
    TablePage ntp(ng.Data());
    ntp.Init();
    auto slot = ntp.Insert(tuple.data(), tuple.size());
    ng.MarkDirty();
    {
        PageGuard lg = PageGuard::Fetch(bpm_, last_);
        TablePage(lg.Data()).SetNextPageId(np);
        lg.MarkDirty();
    }
    last_ = np;
    ++page_count_;
    return RID{np, *slot};
}

bool TableHeap::Get(RID rid, std::string* out) {
    PageGuard g = PageGuard::Fetch(bpm_, rid.page_id);
    return TablePage(g.Data()).Get(static_cast<uint16_t>(rid.slot), out);
}

bool TableHeap::Delete(RID rid) {
    PageGuard g = PageGuard::Fetch(bpm_, rid.page_id);
    bool ok = TablePage(g.Data()).Delete(static_cast<uint16_t>(rid.slot));
    if (ok) {
        g.MarkDirty();
        free_hint_.insert(rid.page_id);
    }
    return ok;
}

RID TableHeap::Update(RID rid, const std::string& tuple) {
    {
        PageGuard g = PageGuard::Fetch(bpm_, rid.page_id);
        if (TablePage(g.Data()).Update(static_cast<uint16_t>(rid.slot), tuple.data(), tuple.size())) {
            g.MarkDirty();
            return rid;
        }
    }
    // does not fit on its page any more: move the tuple
    Delete(rid);
    return Insert(tuple);
}

std::vector<page_id_t> TableHeap::Pages() const {
    std::vector<page_id_t> out;
    for (page_id_t p = first_; p != INVALID_PAGE_ID;) {
        out.push_back(p);
        PageGuard g = PageGuard::Fetch(bpm_, p);
        p = TablePage(g.Data()).NextPageId();
    }
    return out;
}

bool TableHeap::Iterator::Next(RID* rid, std::string* data) {
    while (page_ != INVALID_PAGE_ID) {
        PageGuard g = PageGuard::Fetch(bpm_, page_);
        TablePage tp(g.Data());
        while (slot_ < tp.NumSlots()) {
            uint16_t s = slot_++;
            if (tp.Get(s, data)) {
                *rid = RID{page_, s};
                return true;
            }
        }
        page_ = tp.NextPageId();
        slot_ = 0;
    }
    return false;
}

}  // namespace minidb
