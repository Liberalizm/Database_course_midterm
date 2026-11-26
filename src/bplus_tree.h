// bplus_tree.h - disk-based B+ tree index on 64-bit integer keys.
#pragma once

#include <algorithm>
#include <climits>
#include <vector>

#include "buffer_pool.h"

namespace minidb {

// Index entries are (key, rid) pairs. Making the RID part of the key makes every
// entry unique, which gives us non-unique indexes "for free": all rows with the
// same key are simply adjacent in the leaf chain.
//
// A "wide" tree (used by multi-column indexes) has a second key part: entries
// are (key, key2, rid), ordered by key, then key2, then rid. In a normal tree
// key2 is always 0 and not stored. (key2 is declared last only so that
// IndexKey{key, rid} keeps meaning what it says.)
struct IndexKey {
    int64_t key = 0;
    RID rid;
    int64_t key2 = 0;

    friend bool operator<(const IndexKey& a, const IndexKey& b) {
        if (a.key != b.key) return a.key < b.key;
        if (a.key2 != b.key2) return a.key2 < b.key2;
        return a.rid < b.rid;
    }
    friend bool operator==(const IndexKey& a, const IndexKey& b) {
        return a.key == b.key && a.key2 == b.key2 && a.rid == b.rid;
    }
};

//  Node layouts (each node is one page):
//   header (8 bytes): [is_leaf u8][wide u8][size u16][next_leaf i32]
//   leaf entry     (16 bytes): [key i64][rid.page i32][rid.slot u32]
//   internal entry (24 bytes): [key i64][rid.page i32][rid.slot u32][child i32][pad]
//  A wide tree stores key2 (i64) right after the rid, so its entries are 8 bytes
//  larger (24 / 32) and `wide` is 1 in every node header. Files written before
//  wide trees existed have 0 there (the byte was padding), so they read as narrow.
//  In an internal node with `size` children, key[0] is unused and child[i]
//  holds all entries >= key[i] and < key[i+1].
class BPlusTree {
public:
    static constexpr int LEAF_CAPACITY = static_cast<int>((PAGE_SIZE - 8) / 16);           // 255
    static constexpr int INTERNAL_CAPACITY = static_cast<int>((PAGE_SIZE - 8) / 24);       // 170
    static constexpr int WIDE_LEAF_CAPACITY = static_cast<int>((PAGE_SIZE - 8) / 24);      // 170
    static constexpr int WIDE_INTERNAL_CAPACITY = static_cast<int>((PAGE_SIZE - 8) / 32);  // 127

    // leaf_max / internal_max may be lowered (e.g. in tests) to force deep trees;
    // they are capped at what a page of this tree's layout can hold.
    BPlusTree(BufferPoolManager* bpm, page_id_t root, int leaf_max = LEAF_CAPACITY,
              int internal_max = INTERNAL_CAPACITY, bool wide = false);

    page_id_t Root() const { return root_; }
    bool Wide() const { return wide_; }
    int LeafCapacity() const { return wide_ ? WIDE_LEAF_CAPACITY : LEAF_CAPACITY; }

    bool Insert(const IndexKey& k);  // false if the exact entry already exists
    // false if not found. A node left less than half full borrows from or is
    // merged with a sibling, so the tree shrinks as entries are deleted.
    bool Remove(const IndexKey& k);
    bool Insert(int64_t key, RID rid) { return Insert(IndexKey{key, rid}); }
    bool Remove(int64_t key, RID rid) { return Remove(IndexKey{key, rid}); }

    // True if some entry has this key and a RID different from `except`.
    bool ContainsKey(int64_t key, RID except = RID{}) const;

    class Iterator {
    public:
        Iterator(BufferPoolManager* bpm, page_id_t leaf, int pos, bool wide);
        bool Valid() const { return leaf_ != INVALID_PAGE_ID; }
        const IndexKey& Entry() const { return cur_; }
        void Next();

    private:
        void Normalize();
        BufferPoolManager* bpm_;
        page_id_t leaf_;
        int pos_;
        bool wide_;
        IndexKey cur_;
        size_t hops_ = 0;  // leaves stepped over; more than the file has pages = a cycle
    };

    Iterator Begin() const;  // smallest entry
    // First entry with (entry.key, entry.key2) >= (key, key2).
    Iterator LowerBound(int64_t key, int64_t key2 = INT64_MIN) const;
    int Height() const;
    // Every node page of the tree (used to free them on DROP INDEX / DROP TABLE).
    std::vector<page_id_t> Pages() const;

    // Smallest / largest key (statistics for the planner). False if unknown/empty.
    bool MinKey(int64_t* out) const;
    bool MaxKey(int64_t* out) const;

    // Debug: verify sortedness of the leaf chain and return number of entries.
    size_t CheckAndCount() const;
    // Debug: verify the whole structure (key ranges, equal leaf depth, minimum
    // fill of non-root nodes, the leaf chain) and return the number of entries.
    size_t CheckStructure() const;

private:
    page_id_t FindLeaf(const IndexKey& k, std::vector<page_id_t>* path) const;
    void InsertIntoParent(std::vector<page_id_t>& path, page_id_t left, const IndexKey& sep, page_id_t right);
    void Rebalance(std::vector<page_id_t>& path, page_id_t node);
    // Fewest entries (leaf) / children (internal) a non-root node keeps.
    int LeafMin() const { return std::max(1, (leaf_max_ - 1) / 2); }
    int InternalMin() const { return std::max(2, (internal_max_ - 1) / 2); }

    BufferPoolManager* bpm_;
    page_id_t root_;
    bool wide_;
    int leaf_max_;
    int internal_max_;
};

}  // namespace minidb
