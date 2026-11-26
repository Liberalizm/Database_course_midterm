#include "bplus_tree.h"

#include <algorithm>
#include <climits>

namespace minidb {

namespace {

constexpr size_t HDR = 8;

// Byte layout of the nodes of one tree: narrow (key, rid) or wide (key, rid, key2).
struct Layout {
    bool wide;
    size_t KeyBytes() const { return wide ? 24 : 16; }
    size_t LeafEntry() const { return KeyBytes(); }
    size_t InEntry() const { return KeyBytes() + 8; }  // + child i32 + pad
    int LeafCap() const { return wide ? BPlusTree::WIDE_LEAF_CAPACITY : BPlusTree::LEAF_CAPACITY; }
    int InCap() const { return wide ? BPlusTree::WIDE_INTERNAL_CAPACITY : BPlusTree::INTERNAL_CAPACITY; }
};

bool IsLeaf(const char* d) { return d[0] == 1; }
void InitNode(char* d, bool leaf, const Layout& L) {
    std::memset(d, 0, PAGE_SIZE);
    d[0] = leaf ? 1 : 0;
    d[1] = L.wide ? 1 : 0;
    WriteAs<page_id_t>(d + 4, INVALID_PAGE_ID);
}
int Size(const char* d) { return ReadAs<uint16_t>(d + 2); }
void SetSize(char* d, int n) { WriteAs<uint16_t>(d + 2, static_cast<uint16_t>(n)); }
page_id_t NextLeaf(const char* d) { return ReadAs<page_id_t>(d + 4); }
void SetNextLeaf(char* d, page_id_t p) { WriteAs<page_id_t>(d + 4, p); }

IndexKey ReadKey(const char* p, const Layout& L) {
    IndexKey k;
    k.key = ReadAs<int64_t>(p);
    k.rid.page_id = ReadAs<int32_t>(p + 8);
    k.rid.slot = ReadAs<uint32_t>(p + 12);
    k.key2 = L.wide ? ReadAs<int64_t>(p + 16) : 0;
    return k;
}
void WriteKey(char* p, const IndexKey& k, const Layout& L) {
    WriteAs<int64_t>(p, k.key);
    WriteAs<int32_t>(p + 8, k.rid.page_id);
    WriteAs<uint32_t>(p + 12, k.rid.slot);
    if (L.wide) WriteAs<int64_t>(p + 16, k.key2);
}

char* LeafEntry(char* d, int i, const Layout& L) { return d + HDR + static_cast<size_t>(i) * L.LeafEntry(); }
IndexKey LeafKey(const char* d, int i, const Layout& L) {
    return ReadKey(d + HDR + static_cast<size_t>(i) * L.LeafEntry(), L);
}

char* InEntry(char* d, int i, const Layout& L) { return d + HDR + static_cast<size_t>(i) * L.InEntry(); }
IndexKey InKey(const char* d, int i, const Layout& L) {
    return ReadKey(d + HDR + static_cast<size_t>(i) * L.InEntry(), L);
}
page_id_t Child(const char* d, int i, const Layout& L) {
    return ReadAs<page_id_t>(d + HDR + static_cast<size_t>(i) * L.InEntry() + L.KeyBytes());
}
void SetInEntry(char* d, int i, const IndexKey& k, page_id_t child, const Layout& L) {
    WriteKey(InEntry(d, i, L), k, L);
    WriteAs<page_id_t>(InEntry(d, i, L) + L.KeyBytes(), child);
}

// Fetches a node and validates its header, so that a damaged index file results
// in an error instead of reads beyond the end of the page. A node whose layout
// flag disagrees with the tree's is damaged too (its entries would be misread).
PageGuard FetchNode(BufferPoolManager* bpm, page_id_t pid, const Layout& L) {
    PageGuard g = PageGuard::Fetch(bpm, pid);
    const char* d = g.Data();
    bool leaf = d[0] == 1;
    int cap = leaf ? L.LeafCap() : L.InCap();
    if ((d[0] != 0 && d[0] != 1) || d[1] != (L.wide ? 1 : 0) || Size(d) > cap || (!leaf && Size(d) < 1))
        throw DbError("index page " + std::to_string(pid) + " is corrupt");
    return g;
}

// Largest i >= 1 with key[i] <= k, or 0 if none (binary search).
int ChildIndexFor(const char* d, const IndexKey& k, const Layout& L) {
    int lo = 1, hi = Size(d) - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (!(k < InKey(d, mid, L))) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

// First position i with leaf_key[i] >= k.
int LeafLowerBound(const char* d, const IndexKey& k, const Layout& L) {
    int lo = 0, hi = Size(d);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (LeafKey(d, mid, L) < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

}  // namespace

BPlusTree::BPlusTree(BufferPoolManager* bpm, page_id_t root, int leaf_max, int internal_max, bool wide)
    : bpm_(bpm),
      root_(root),
      wide_(wide),
      leaf_max_(std::clamp(leaf_max, 3, Layout{wide}.LeafCap())),
      internal_max_(std::clamp(internal_max, 4, Layout{wide}.InCap())) {}

page_id_t BPlusTree::FindLeaf(const IndexKey& k, std::vector<page_id_t>* path) const {
    const Layout L{wide_};
    page_id_t pid = root_;
    while (true) {
        PageGuard g = FetchNode(bpm_, pid, L);
        if (IsLeaf(g.Data())) return pid;
        if (path) path->push_back(pid);
        pid = Child(g.Data(), ChildIndexFor(g.Data(), k, L), L);
    }
}

bool BPlusTree::Insert(const IndexKey& k) {
    const Layout L{wide_};
    if (!wide_ && k.key2 != 0) throw DbError("internal: second key part in a single-key index");
    if (root_ == INVALID_PAGE_ID) {  // empty tree: root is a single leaf
        page_id_t pid;
        PageGuard g = PageGuard::New(bpm_, &pid);
        InitNode(g.Data(), true, L);
        WriteKey(LeafEntry(g.Data(), 0, L), k, L);
        SetSize(g.Data(), 1);
        g.MarkDirty();
        root_ = pid;
        return true;
    }

    std::vector<page_id_t> path;
    page_id_t leaf_id = FindLeaf(k, &path);
    page_id_t new_id = INVALID_PAGE_ID;
    IndexKey sep;
    {
        PageGuard g = FetchNode(bpm_, leaf_id, L);
        char* d = g.Data();
        int n = Size(d);
        int pos = LeafLowerBound(d, k, L);
        if (pos < n && LeafKey(d, pos, L) == k) return false;
        std::memmove(LeafEntry(d, pos + 1, L), LeafEntry(d, pos, L), static_cast<size_t>(n - pos) * L.LeafEntry());
        WriteKey(LeafEntry(d, pos, L), k, L);
        SetSize(d, ++n);
        g.MarkDirty();
        if (n < leaf_max_) return true;

        // Leaf is full -> split it in half. The first key of the right half is
        // copied up into the parent as the separator.
        PageGuard ng = PageGuard::New(bpm_, &new_id);
        char* nd = ng.Data();
        InitNode(nd, true, L);
        int mid = n / 2;
        std::memcpy(LeafEntry(nd, 0, L), LeafEntry(d, mid, L), static_cast<size_t>(n - mid) * L.LeafEntry());
        SetSize(nd, n - mid);
        SetSize(d, mid);
        SetNextLeaf(nd, NextLeaf(d));
        SetNextLeaf(d, new_id);
        ng.MarkDirty();
        sep = LeafKey(nd, 0, L);
    }
    InsertIntoParent(path, leaf_id, sep, new_id);
    return true;
}

void BPlusTree::InsertIntoParent(std::vector<page_id_t>& path, page_id_t left, const IndexKey& sep,
                                 page_id_t right) {
    const Layout L{wide_};
    if (path.empty()) {  // the root was split -> tree grows by one level
        page_id_t pid;
        PageGuard g = PageGuard::New(bpm_, &pid);
        char* d = g.Data();
        InitNode(d, false, L);
        SetInEntry(d, 0, IndexKey{}, left, L);
        SetInEntry(d, 1, sep, right, L);
        SetSize(d, 2);
        g.MarkDirty();
        root_ = pid;
        return;
    }
    page_id_t parent = path.back();
    path.pop_back();
    page_id_t new_id;
    IndexKey up;
    {
        PageGuard g = FetchNode(bpm_, parent, L);
        char* d = g.Data();
        int n = Size(d);
        int idx = -1;
        for (int i = 0; i < n; ++i) {
            if (Child(d, i, L) == left) {
                idx = i;
                break;
            }
        }
        if (idx < 0) throw DbError("index corruption: child not found in parent");
        std::memmove(InEntry(d, idx + 2, L), InEntry(d, idx + 1, L), static_cast<size_t>(n - idx - 1) * L.InEntry());
        SetInEntry(d, idx + 1, sep, right, L);
        SetSize(d, ++n);
        g.MarkDirty();
        if (n < internal_max_) return;

        // Internal node full -> split. The middle key moves up (not copied).
        PageGuard ng = PageGuard::New(bpm_, &new_id);
        char* nd = ng.Data();
        InitNode(nd, false, L);
        int mid = n / 2;
        std::memcpy(InEntry(nd, 0, L), InEntry(d, mid, L), static_cast<size_t>(n - mid) * L.InEntry());
        SetSize(nd, n - mid);
        SetSize(d, mid);
        up = InKey(nd, 0, L);
        ng.MarkDirty();
    }
    InsertIntoParent(path, parent, up, new_id);
}

bool BPlusTree::Remove(const IndexKey& k) {
    const Layout L{wide_};
    if (root_ == INVALID_PAGE_ID) return false;
    page_id_t leaf = FindLeaf(k, nullptr);
    PageGuard g = FetchNode(bpm_, leaf, L);
    char* d = g.Data();
    int n = Size(d);
    int pos = LeafLowerBound(d, k, L);
    if (pos >= n || !(LeafKey(d, pos, L) == k)) return false;
    std::memmove(LeafEntry(d, pos, L), LeafEntry(d, pos + 1, L), static_cast<size_t>(n - pos - 1) * L.LeafEntry());
    SetSize(d, n - 1);
    g.MarkDirty();
    return true;
}

bool BPlusTree::ContainsKey(int64_t key, RID except) const {
    for (Iterator it = LowerBound(key); it.Valid() && it.Entry().key == key; it.Next()) {
        if (it.Entry().rid != except) return true;
    }
    return false;
}

BPlusTree::Iterator BPlusTree::Begin() const {
    const Layout L{wide_};
    if (root_ == INVALID_PAGE_ID) return Iterator(bpm_, INVALID_PAGE_ID, 0, wide_);
    page_id_t pid = root_;
    while (true) {
        PageGuard g = FetchNode(bpm_, pid, L);
        if (IsLeaf(g.Data())) break;
        pid = Child(g.Data(), 0, L);
    }
    return Iterator(bpm_, pid, 0, wide_);
}

BPlusTree::Iterator BPlusTree::LowerBound(int64_t key, int64_t key2) const {
    const Layout L{wide_};
    if (root_ == INVALID_PAGE_ID) return Iterator(bpm_, INVALID_PAGE_ID, 0, wide_);
    IndexKey k{key, RID{INT32_MIN, 0}, key2};
    page_id_t leaf = FindLeaf(k, nullptr);
    int pos;
    {
        PageGuard g = FetchNode(bpm_, leaf, L);
        pos = LeafLowerBound(g.Data(), k, L);
    }
    return Iterator(bpm_, leaf, pos, wide_);
}

int BPlusTree::Height() const {
    const Layout L{wide_};
    if (root_ == INVALID_PAGE_ID) return 0;
    int h = 1;
    page_id_t pid = root_;
    while (true) {
        PageGuard g = FetchNode(bpm_, pid, L);
        if (IsLeaf(g.Data())) return h;
        pid = Child(g.Data(), 0, L);
        ++h;
    }
}

std::vector<page_id_t> BPlusTree::Pages() const {
    const Layout L{wide_};
    std::vector<page_id_t> out;
    if (root_ == INVALID_PAGE_ID) return out;
    std::vector<page_id_t> stack{root_};
    while (!stack.empty()) {
        page_id_t pid = stack.back();
        stack.pop_back();
        out.push_back(pid);
        PageGuard g = FetchNode(bpm_, pid, L);
        if (IsLeaf(g.Data())) continue;
        for (int i = 0; i < Size(g.Data()); ++i) stack.push_back(Child(g.Data(), i, L));
    }
    return out;
}

bool BPlusTree::MinKey(int64_t* out) const {
    Iterator it = Begin();
    if (!it.Valid()) return false;
    *out = it.Entry().key;
    return true;
}

bool BPlusTree::MaxKey(int64_t* out) const {
    const Layout L{wide_};
    if (root_ == INVALID_PAGE_ID) return false;
    // Lazy deletes can leave the rightmost leaves empty, so this is a right-to-left
    // depth-first search for the last non-empty leaf, not a single descent. Leaves
    // have no back pointers, and usually the first leaf visited already answers.
    std::vector<page_id_t> stack{root_};
    while (!stack.empty()) {
        page_id_t pid = stack.back();
        stack.pop_back();
        PageGuard g = FetchNode(bpm_, pid, L);
        const char* d = g.Data();
        int n = Size(d);
        if (IsLeaf(d)) {
            if (n == 0) continue;
            *out = LeafKey(d, n - 1, L).key;
            return true;
        }
        for (int i = 0; i < n; ++i) stack.push_back(Child(d, i, L));  // rightmost child on top
    }
    return false;  // every entry was deleted
}

size_t BPlusTree::CheckAndCount() const {
    size_t n = 0;
    bool first = true;
    IndexKey prev;
    for (Iterator it = Begin(); it.Valid(); it.Next()) {
        if (!first && !(prev < it.Entry())) throw DbError("index corruption: leaf chain not sorted");
        prev = it.Entry();
        first = false;
        ++n;
    }
    return n;
}

BPlusTree::Iterator::Iterator(BufferPoolManager* bpm, page_id_t leaf, int pos, bool wide)
    : bpm_(bpm), leaf_(leaf), pos_(pos), wide_(wide) {
    Normalize();
}

void BPlusTree::Iterator::Next() {
    ++pos_;
    Normalize();
}

// Moves forward over exhausted / empty leaves until positioned on a real entry.
void BPlusTree::Iterator::Normalize() {
    const Layout L{wide_};
    while (leaf_ != INVALID_PAGE_ID) {
        PageGuard g = FetchNode(bpm_, leaf_, L);
        if (pos_ < Size(g.Data())) {
            cur_ = LeafKey(g.Data(), pos_, L);
            return;
        }
        leaf_ = NextLeaf(g.Data());
        if (++hops_ > static_cast<size_t>(bpm_->Disk()->NumPages()))
            throw DbError("index is corrupt (cycle in the leaf chain)");
        pos_ = 0;
    }
}

}  // namespace minidb
