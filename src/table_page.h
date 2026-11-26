// table_page.h - slotted page layout for storing variable-length tuples.
#pragma once

#include <optional>
#include <string>

#include "common.h"

namespace minidb {

//  Slotted page layout (4096 bytes):
//
//  +-----------+-----------+----------+--------------------+
//  | next_page | num_slots | free_end | slot[0] slot[1] .. |  -> grows to the right
//  |  int32    |  uint16   |  uint16  | {off u16, len u16} |
//  +-----------+-----------+----------+--------------------+
//  |                 ... free space ...                      |
//  +---------------------------------------------------------+
//  |              ... tuple data (grows to the left) ...    |
//  +---------------------------------------------------------+
//
//  A slot with off == 0 is empty (deleted). Slot numbers never change while a
//  tuple is alive, so RID = (page, slot) stays valid even after compaction.
class TablePage {
public:
    static constexpr size_t HEADER = 8;
    static constexpr size_t SLOT = 4;
    static constexpr size_t MAX_TUPLE = PAGE_SIZE - HEADER - SLOT;

    explicit TablePage(char* data) : d_(data) {}

    void Init();

    page_id_t NextPageId() const { return ReadAs<page_id_t>(d_); }
    void SetNextPageId(page_id_t p) { WriteAs<page_id_t>(d_, p); }
    uint16_t NumSlots() const { return ReadAs<uint16_t>(d_ + 4); }

    // Contiguous free bytes between slot array and tuple data.
    size_t FreeSpace() const { return FreeEnd() - (HEADER + NumSlots() * SLOT); }

    std::optional<uint16_t> Insert(const char* data, size_t len);
    bool Get(uint16_t slot, std::string* out) const;
    bool Delete(uint16_t slot);
    // Updates in place (the slot number is preserved). Returns false if the new
    // version does not fit on this page even after compaction.
    bool Update(uint16_t slot, const char* data, size_t len);
    // Moves all live tuples to the end of the page, removing holes.
    void Compact();
    size_t LiveCount() const;
    // Throws DbError if the header or a slot points outside the page (damaged file).
    void CheckIntegrity() const;
    // O(1) variant for single-slot operations: the header and `slot` only.
    void CheckSlot(uint16_t slot) const;

private:
    uint16_t FreeEnd() const { return ReadAs<uint16_t>(d_ + 6); }
    void SetFreeEnd(uint16_t v) { WriteAs<uint16_t>(d_ + 6, v); }
    void SetNumSlots(uint16_t n) { WriteAs<uint16_t>(d_ + 4, n); }
    uint16_t SlotOff(uint16_t i) const { return ReadAs<uint16_t>(d_ + HEADER + i * SLOT); }
    uint16_t SlotLen(uint16_t i) const { return ReadAs<uint16_t>(d_ + HEADER + i * SLOT + 2); }
    void SetSlot(uint16_t i, uint16_t off, uint16_t len) {
        WriteAs<uint16_t>(d_ + HEADER + i * SLOT, off);
        WriteAs<uint16_t>(d_ + HEADER + i * SLOT + 2, len);
    }
    size_t LiveBytes() const;

    char* d_;
};

}  // namespace minidb
