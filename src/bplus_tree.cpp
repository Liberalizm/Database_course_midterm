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
    std::vector<page_id_t> path;
    page_id_t leaf = FindLeaf(k, &path);
    int n;
    {
        PageGuard g = FetchNode(bpm_, leaf, L);
        char* d = g.Data();
        n = Size(d);
        int pos = LeafLowerBound(d, k, L);
        if (pos >= n || !(LeafKey(d, pos, L) == k)) return false;
        std::memmove(LeafEntry(d, pos, L), LeafEntry(d, pos + 1, L), static_cast<size_t>(n - pos - 1) * L.LeafEntry());
        SetSize(d, --n);
        g.MarkDirty();
    }
    // The root may hold any number of entries (an empty root leaf is an empty tree).
    if (!path.empty() && n < LeafMin()) Rebalance(path, leaf);
    return true;
}

// `node` (a child of path.back()) has fallen below its minimum size. Together
// with an adjacent sibling under the same parent it either fits into one node
// (merge: the right one is freed and its separator removed from the parent) or
// is split evenly again (redistribute: only the separator changes). Siblings
// under one parent are also neighbours in the leaf chain, so the chain stays
// intact. A merge can make the parent underflow in turn; a root left with a
// single child is replaced by that child, so the tree shrinks by one level.
void BPlusTree::Rebalance(std::vector<page_id_t>& path, page_id_t node) {
    const Layout L{wide_};
    const page_id_t parent = path.back();
    path.pop_back();
    page_id_t freed = INVALID_PAGE_ID;
    int parent_size;
    {
        PageGuard pg = FetchNode(bpm_, parent, L);
        char* p = pg.Data();
        const int pn = Size(p);
        int idx = -1;
        for (int i = 0; i < pn; ++i)
            if (Child(p, i, L) == node) idx = i;
        if (idx < 0) throw DbError("index corruption: child not found in parent");
        if (pn < 2) return;  // no sibling to work with
        const int s = idx > 0 ? idx : idx + 1;  // the right node of the pair is child s
        const page_id_t left_id = Child(p, s - 1, L), right_id = Child(p, s, L);
        PageGuard lg = FetchNode(bpm_, left_id, L), rg = FetchNode(bpm_, right_id, L);
        char* ld = lg.Data();
        char* rd = rg.Data();
        if (IsLeaf(ld) != IsLeaf(rd)) throw DbError("index corruption: siblings of different kinds");
        const int ln = Size(ld), rn = Size(rd);

        if (IsLeaf(ld)) {
            std::vector<IndexKey> all;
            all.reserve(static_cast<size_t>(ln + rn));
            for (int i = 0; i < ln; ++i) all.push_back(LeafKey(ld, i, L));
            for (int i = 0; i < rn; ++i) all.push_back(LeafKey(rd, i, L));
            const int t = static_cast<int>(all.size());
            if (t < leaf_max_) {  // merge into the left leaf
                for (int i = 0; i < t; ++i) WriteKey(LeafEntry(ld, i, L), all[static_cast<size_t>(i)], L);
                SetSize(ld, t);
                SetNextLeaf(ld, NextLeaf(rd));
                freed = right_id;
            } else {  // redistribute
                const int h = t / 2;
                for (int i = 0; i < h; ++i) WriteKey(LeafEntry(ld, i, L), all[static_cast<size_t>(i)], L);
                for (int i = h; i < t; ++i) WriteKey(LeafEntry(rd, i - h, L), all[static_cast<size_t>(i)], L);
                SetSize(ld, h);
                SetSize(rd, t - h);
                SetInEntry(p, s, all[static_cast<size_t>(h)], right_id, L);
            }
        } else {
            // Internal: the parent's separator comes down in front of the right
            // node's first child (whose own key slot is unused).
            std::vector<std::pair<IndexKey, page_id_t>> all;
            all.reserve(static_cast<size_t>(ln + rn));
            for (int i = 0; i < ln; ++i) all.emplace_back(InKey(ld, i, L), Child(ld, i, L));
            all.emplace_back(InKey(p, s, L), Child(rd, 0, L));
            for (int i = 1; i < rn; ++i) all.emplace_back(InKey(rd, i, L), Child(rd, i, L));
            const int t = static_cast<int>(all.size());
            auto put = [&](char* d, int from, int to) {
                for (int i = from; i < to; ++i)
                    SetInEntry(d, i - from, all[static_cast<size_t>(i)].first, all[static_cast<size_t>(i)].second, L);
                SetSize(d, to - from);
            };
            if (t < internal_max_) {
                put(ld, 0, t);
                freed = right_id;
            } else {
                const int h = t / 2;
                put(ld, 0, h);
                put(rd, h, t);
                SetInEntry(p, s, all[static_cast<size_t>(h)].first, right_id, L);
            }
        }
        lg.MarkDirty();
        rg.MarkDirty();
        if (freed != INVALID_PAGE_ID) {  // drop the right node's entry from the parent
            std::memmove(InEntry(p, s, L), InEntry(p, s + 1, L), static_cast<size_t>(pn - s - 1) * L.InEntry());
            SetSize(p, pn - 1);
        }
        pg.MarkDirty();
        parent_size = Size(p);
    }
    if (freed != INVALID_PAGE_ID) bpm_->DeletePage(freed);  // unpinned now

    if (path.empty()) {  // `parent` is the root
        if (parent_size == 1) {
            page_id_t only;
            {
                PageGuard pg = FetchNode(bpm_, parent, L);
                only = Child(pg.Data(), 0, L);
            }
            root_ = only;
            bpm_->DeletePage(parent);
        }
    } else if (parent_size < InternalMin()) {
        Rebalance(path, parent);
    }
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
    // Trees written before deletes rebalanced (and a root leaf) can hold empty
    // leaves, so this is a right-to-left depth-first search for the last non-empty
    // leaf, not a single descent. Usually the first leaf visited already answers.
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

namespace {

struct StructureCheck {
    StructureCheck(BufferPoolManager* b, Layout l, int lmin, int imin)
        : bpm(b), L(l), leaf_min(lmin), internal_min(imin) {}
    BufferPoolManager* bpm;
    Layout L;
    int leaf_min, internal_min;
    int leaf_depth = -1;
    std::vector<page_id_t> leaves;  // in key order
    size_t entries = 0;

    [[noreturn]] static void Fail(page_id_t pid, const std::string& what) {
        throw DbError("index structure broken at page " + std::to_string(pid) + ": " + what);
    }

    // Every entry of the subtree at `pid` must lie in [lo, hi) (unbounded when null).
    void Visit(page_id_t pid, int depth, bool is_root, const IndexKey* lo, const IndexKey* hi) {
        PageGuard g = FetchNode(bpm, pid, L);
        const char* d = g.Data();
        const int n = Size(d);
        if (IsLeaf(d)) {
            if (leaf_depth < 0) leaf_depth = depth;
            if (depth != leaf_depth) Fail(pid, "leaves at different depths");
            if (!is_root && n < leaf_min) Fail(pid, "leaf underfull");
            for (int i = 0; i < n; ++i) {
                IndexKey k = LeafKey(d, i, L);
                if ((i > 0 && !(LeafKey(d, i - 1, L) < k)) || (lo && k < *lo) || (hi && !(k < *hi)))
                    Fail(pid, "leaf entry out of order");
            }
            leaves.push_back(pid);
            entries += static_cast<size_t>(n);
            return;
        }
        if (!is_root && n < internal_min) Fail(pid, "internal node underfull");
        if (is_root && n < 2) Fail(pid, "internal root with a single child");
        std::vector<std::pair<IndexKey, page_id_t>> kids;
        for (int i = 0; i < n; ++i) kids.emplace_back(InKey(d, i, L), Child(d, i, L));
        g = PageGuard();  // unpin before descending
        for (int i = 0; i < n; ++i) {
            const IndexKey* klo = i == 0 ? lo : &kids[static_cast<size_t>(i)].first;
            const IndexKey* khi = i + 1 < n ? &kids[static_cast<size_t>(i) + 1].first : hi;
            if (klo && khi && !(*klo < *khi)) Fail(pid, "separators out of order");
            Visit(kids[static_cast<size_t>(i)].second, depth + 1, false, klo, khi);
        }
    }
};

}  // namespace

size_t BPlusTree::CheckStructure() const {
    if (root_ == INVALID_PAGE_ID) return 0;
    StructureCheck c(bpm_, Layout{wide_}, LeafMin(), InternalMin());
    c.Visit(root_, 0, true, nullptr, nullptr);
    // The leaf chain must link exactly the leaves, in key order.
    for (size_t i = 0; i < c.leaves.size(); ++i) {
        PageGuard g = FetchNode(bpm_, c.leaves[i], Layout{wide_});
        page_id_t want = i + 1 < c.leaves.size() ? c.leaves[i + 1] : INVALID_PAGE_ID;
        if (NextLeaf(g.Data()) != want) StructureCheck::Fail(c.leaves[i], "leaf chain broken");
    }
    return c.entries;
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
