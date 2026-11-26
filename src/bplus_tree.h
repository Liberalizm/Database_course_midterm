// bplus_tree.h - disk-based B+ tree index on 64-bit integer keys.
#pragma once

#include <vector>

#include "buffer_pool.h"

namespace minidb {

// Index entries are (key, rid) pairs. Making the RID part of the key makes every
// entry unique, which gives us non-unique indexes "for free": all rows with the
// same key are simply adjacent in the leaf chain.
struct IndexKey {
    int64_t key = 0;
    RID rid;

    friend bool operator<(const IndexKey& a, const IndexKey& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.rid < b.rid;
    }
    friend bool operator==(const IndexKey& a, const IndexKey& b) { return a.key == b.key && a.rid == b.rid; }
};

//  Node layouts (each node is one page):
//   header (8 bytes): [is_leaf u8][pad u8][size u16][next_leaf i32]
//   leaf entry    (16 bytes): [key i64][rid.page i32][rid.slot u32]
//   internal entry(24 bytes): [key i64][rid.page i32][rid.slot u32][child i32][pad]
//  In an internal node with `size` children, key[0] is unused and child[i]
//  holds all entries >= key[i] and < key[i+1].
class BPlusTree {
public:
    static constexpr int LEAF_CAPACITY = static_cast<int>((PAGE_SIZE - 8) / 16);      // 255
    static constexpr int INTERNAL_CAPACITY = static_cast<int>((PAGE_SIZE - 8) / 24);  // 170

    // leaf_max / internal_max may be lowered (e.g. in tests) to force deep trees.
    BPlusTree(BufferPoolManager* bpm, page_id_t root, int leaf_max = LEAF_CAPACITY,
              int internal_max = INTERNAL_CAPACITY);

    page_id_t Root() const { return root_; }

    bool Insert(int64_t key, RID rid);  // false if the exact (key, rid) already exists
    bool Remove(int64_t key, RID rid);  // false if not found (no rebalancing: lazy delete)

    // True if some entry has this key and a RID different from `except`.
    bool ContainsKey(int64_t key, RID except = RID{}) const;

    class Iterator {
    public:
        Iterator(BufferPoolManager* bpm, page_id_t leaf, int pos);
        bool Valid() const { return leaf_ != INVALID_PAGE_ID; }
        const IndexKey& Entry() const { return cur_; }
        void Next();

    private:
        void Normalize();
        BufferPoolManager* bpm_;
        page_id_t leaf_;
        int pos_;
        IndexKey cur_;
        size_t hops_ = 0;  // leaves stepped over; more than the file has pages = a cycle
    };

    Iterator Begin() const;                  // smallest entry
    Iterator LowerBound(int64_t key) const;  // first entry with entry.key >= key
    int Height() const;
    // Every node page of the tree (used to free them on DROP INDEX / DROP TABLE).
    std::vector<page_id_t> Pages() const;

    // Smallest / largest key (statistics for the planner). False if unknown/empty.
    bool MinKey(int64_t* out) const;
    bool MaxKey(int64_t* out) const;

    // Debug: verify sortedness of the leaf chain and return number of entries.
    size_t CheckAndCount() const;

private:
    page_id_t FindLeaf(const IndexKey& k, std::vector<page_id_t>* path) const;
    void InsertIntoParent(std::vector<page_id_t>& path, page_id_t left, const IndexKey& sep, page_id_t right);

    BufferPoolManager* bpm_;
    page_id_t root_;
    int leaf_max_;
    int internal_max_;
};

}  // namespace minidb
