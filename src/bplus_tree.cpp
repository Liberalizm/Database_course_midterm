#include "bplus_tree.h"

#include <algorithm>
#include <climits>

namespace minidb {

namespace {

constexpr size_t HDR = 8;
constexpr size_t LEAF_ENTRY = 16;
constexpr size_t IN_ENTRY = 24;

bool IsLeaf(const char* d) { return d[0] == 1; }
void InitNode(char* d, bool leaf) {
    std::memset(d, 0, PAGE_SIZE);
    d[0] = leaf ? 1 : 0;
    WriteAs<page_id_t>(d + 4, INVALID_PAGE_ID);
}
int Size(const char* d) { return ReadAs<uint16_t>(d + 2); }
void SetSize(char* d, int n) { WriteAs<uint16_t>(d + 2, static_cast<uint16_t>(n)); }
page_id_t NextLeaf(const char* d) { return ReadAs<page_id_t>(d + 4); }
void SetNextLeaf(char* d, page_id_t p) { WriteAs<page_id_t>(d + 4, p); }

IndexKey ReadKey(const char* p) {
    IndexKey k;
    k.key = ReadAs<int64_t>(p);
    k.rid.page_id = ReadAs<int32_t>(p + 8);
    k.rid.slot = ReadAs<uint32_t>(p + 12);
    return k;
}
void WriteKey(char* p, const IndexKey& k) {
    WriteAs<int64_t>(p, k.key);
    WriteAs<int32_t>(p + 8, k.rid.page_id);
    WriteAs<uint32_t>(p + 12, k.rid.slot);
}

char* LeafEntry(char* d, int i) { return d + HDR + static_cast<size_t>(i) * LEAF_ENTRY; }
IndexKey LeafKey(const char* d, int i) { return ReadKey(d + HDR + static_cast<size_t>(i) * LEAF_ENTRY); }

char* InEntry(char* d, int i) { return d + HDR + static_cast<size_t>(i) * IN_ENTRY; }
IndexKey InKey(const char* d, int i) { return ReadKey(d + HDR + static_cast<size_t>(i) * IN_ENTRY); }
page_id_t Child(const char* d, int i) { return ReadAs<page_id_t>(d + HDR + static_cast<size_t>(i) * IN_ENTRY + 16); }
void SetInEntry(char* d, int i, const IndexKey& k, page_id_t child) {
    WriteKey(InEntry(d, i), k);
    WriteAs<page_id_t>(InEntry(d, i) + 16, child);
}

// Largest i >= 1 with key[i] <= k, or 0 if none (binary search).
int ChildIndexFor(const char* d, const IndexKey& k) {
    int lo = 1, hi = Size(d) - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (!(k < InKey(d, mid))) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

// First position i with leaf_key[i] >= k.
int LeafLowerBound(const char* d, const IndexKey& k) {
    int lo = 0, hi = Size(d);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (LeafKey(d, mid) < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

}  // namespace

BPlusTree::BPlusTree(BufferPoolManager* bpm, page_id_t root, int leaf_max, int internal_max)
    : bpm_(bpm),
      root_(root),
      leaf_max_(std::clamp(leaf_max, 3, LEAF_CAPACITY)),
      internal_max_(std::clamp(internal_max, 4, INTERNAL_CAPACITY)) {}

page_id_t BPlusTree::FindLeaf(const IndexKey& k, std::vector<page_id_t>* path) const {
    page_id_t pid = root_;
    while (true) {
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        if (IsLeaf(g.Data())) return pid;
        if (path) path->push_back(pid);
        pid = Child(g.Data(), ChildIndexFor(g.Data(), k));
    }
}

bool BPlusTree::Insert(int64_t key, RID rid) {
    IndexKey k{key, rid};
    if (root_ == INVALID_PAGE_ID) {  // empty tree: root is a single leaf
        page_id_t pid;
        PageGuard g = PageGuard::New(bpm_, &pid);
        InitNode(g.Data(), true);
        WriteKey(LeafEntry(g.Data(), 0), k);
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
        PageGuard g = PageGuard::Fetch(bpm_, leaf_id);
        char* d = g.Data();
        int n = Size(d);
        int pos = LeafLowerBound(d, k);
        if (pos < n && LeafKey(d, pos) == k) return false;
        std::memmove(LeafEntry(d, pos + 1), LeafEntry(d, pos), static_cast<size_t>(n - pos) * LEAF_ENTRY);
        WriteKey(LeafEntry(d, pos), k);
        SetSize(d, ++n);
        g.MarkDirty();
        if (n < leaf_max_) return true;

        // Leaf is full -> split it in half. The first key of the right half is
        // copied up into the parent as the separator.
        PageGuard ng = PageGuard::New(bpm_, &new_id);
        char* nd = ng.Data();
        InitNode(nd, true);
        int mid = n / 2;
        std::memcpy(LeafEntry(nd, 0), LeafEntry(d, mid), static_cast<size_t>(n - mid) * LEAF_ENTRY);
        SetSize(nd, n - mid);
        SetSize(d, mid);
        SetNextLeaf(nd, NextLeaf(d));
        SetNextLeaf(d, new_id);
        ng.MarkDirty();
        sep = LeafKey(nd, 0);
    }
    InsertIntoParent(path, leaf_id, sep, new_id);
    return true;
}

void BPlusTree::InsertIntoParent(std::vector<page_id_t>& path, page_id_t left, const IndexKey& sep,
                                 page_id_t right) {
    if (path.empty()) {  // the root was split -> tree grows by one level
        page_id_t pid;
        PageGuard g = PageGuard::New(bpm_, &pid);
        char* d = g.Data();
        InitNode(d, false);
        SetInEntry(d, 0, IndexKey{}, left);
        SetInEntry(d, 1, sep, right);
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
        PageGuard g = PageGuard::Fetch(bpm_, parent);
        char* d = g.Data();
        int n = Size(d);
        int idx = -1;
        for (int i = 0; i < n; ++i) {
            if (Child(d, i) == left) {
                idx = i;
                break;
            }
        }
        if (idx < 0) throw DbError("index corruption: child not found in parent");
        std::memmove(InEntry(d, idx + 2), InEntry(d, idx + 1), static_cast<size_t>(n - idx - 1) * IN_ENTRY);
        SetInEntry(d, idx + 1, sep, right);
        SetSize(d, ++n);
        g.MarkDirty();
        if (n < internal_max_) return;

        // Internal node full -> split. The middle key moves up (not copied).
        PageGuard ng = PageGuard::New(bpm_, &new_id);
        char* nd = ng.Data();
        InitNode(nd, false);
        int mid = n / 2;
        std::memcpy(InEntry(nd, 0), InEntry(d, mid), static_cast<size_t>(n - mid) * IN_ENTRY);
        SetSize(nd, n - mid);
        SetSize(d, mid);
        up = InKey(nd, 0);
        ng.MarkDirty();
    }
    InsertIntoParent(path, parent, up, new_id);
}

bool BPlusTree::Remove(int64_t key, RID rid) {
    if (root_ == INVALID_PAGE_ID) return false;
    IndexKey k{key, rid};
    page_id_t leaf = FindLeaf(k, nullptr);
    PageGuard g = PageGuard::Fetch(bpm_, leaf);
    char* d = g.Data();
    int n = Size(d);
    int pos = LeafLowerBound(d, k);
    if (pos >= n || !(LeafKey(d, pos) == k)) return false;
    std::memmove(LeafEntry(d, pos), LeafEntry(d, pos + 1), static_cast<size_t>(n - pos - 1) * LEAF_ENTRY);
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
    if (root_ == INVALID_PAGE_ID) return Iterator(bpm_, INVALID_PAGE_ID, 0);
    page_id_t pid = root_;
    while (true) {
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        if (IsLeaf(g.Data())) break;
        pid = Child(g.Data(), 0);
    }
    return Iterator(bpm_, pid, 0);
}

BPlusTree::Iterator BPlusTree::LowerBound(int64_t key) const {
    if (root_ == INVALID_PAGE_ID) return Iterator(bpm_, INVALID_PAGE_ID, 0);
    IndexKey k{key, RID{INT32_MIN, 0}};
    page_id_t leaf = FindLeaf(k, nullptr);
    int pos;
    {
        PageGuard g = PageGuard::Fetch(bpm_, leaf);
        pos = LeafLowerBound(g.Data(), k);
    }
    return Iterator(bpm_, leaf, pos);
}

int BPlusTree::Height() const {
    if (root_ == INVALID_PAGE_ID) return 0;
    int h = 1;
    page_id_t pid = root_;
    while (true) {
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        if (IsLeaf(g.Data())) return h;
        pid = Child(g.Data(), 0);
        ++h;
    }
}

std::vector<page_id_t> BPlusTree::Pages() const {
    std::vector<page_id_t> out;
    if (root_ == INVALID_PAGE_ID) return out;
    std::vector<page_id_t> stack{root_};
    while (!stack.empty()) {
        page_id_t pid = stack.back();
        stack.pop_back();
        out.push_back(pid);
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        if (IsLeaf(g.Data())) continue;
        for (int i = 0; i < Size(g.Data()); ++i) stack.push_back(Child(g.Data(), i));
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
    if (root_ == INVALID_PAGE_ID) return false;
    // Lazy deletes can leave the rightmost leaves empty, so this is a right-to-left
    // depth-first search for the last non-empty leaf, not a single descent. Leaves
    // have no back pointers, and usually the first leaf visited already answers.
    std::vector<page_id_t> stack{root_};
    while (!stack.empty()) {
        page_id_t pid = stack.back();
        stack.pop_back();
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        const char* d = g.Data();
        int n = Size(d);
        if (IsLeaf(d)) {
            if (n == 0) continue;
            *out = LeafKey(d, n - 1).key;
            return true;
        }
        for (int i = 0; i < n; ++i) stack.push_back(Child(d, i));  // rightmost child on top
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

BPlusTree::Iterator::Iterator(BufferPoolManager* bpm, page_id_t leaf, int pos)
    : bpm_(bpm), leaf_(leaf), pos_(pos) {
    Normalize();
}

void BPlusTree::Iterator::Next() {
    ++pos_;
    Normalize();
}

// Moves forward over exhausted / empty leaves until positioned on a real entry.
void BPlusTree::Iterator::Normalize() {
    while (leaf_ != INVALID_PAGE_ID) {
        PageGuard g = PageGuard::Fetch(bpm_, leaf_);
        if (pos_ < Size(g.Data())) {
            cur_ = LeafKey(g.Data(), pos_);
            return;
        }
        leaf_ = NextLeaf(g.Data());
        pos_ = 0;
    }
}

}  // namespace minidb
