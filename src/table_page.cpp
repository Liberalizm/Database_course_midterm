#include "table_page.h"

namespace minidb {

void TablePage::Init() {
    std::memset(d_, 0, PAGE_SIZE);
    SetNextPageId(INVALID_PAGE_ID);
    SetNumSlots(0);
    SetFreeEnd(static_cast<uint16_t>(PAGE_SIZE));
}

// A damaged file must produce an error, not out-of-bounds memory access: the
// header and every live slot have to describe ranges inside this page.
void TablePage::CheckIntegrity() const {
    size_t slots_end = HEADER + static_cast<size_t>(NumSlots()) * SLOT;
    if (slots_end > PAGE_SIZE || FreeEnd() < slots_end || FreeEnd() > PAGE_SIZE)
        throw DbError("table page is corrupt (bad header)");
    size_t live = 0;
    for (uint16_t i = 0; i < NumSlots(); ++i) {
        uint16_t off = SlotOff(i);
        if (off == 0) continue;
        if (off < FreeEnd() || SlotLen(i) == 0 || static_cast<size_t>(off) + SlotLen(i) > PAGE_SIZE)
            throw DbError("table page is corrupt (bad slot " + std::to_string(i) + ")");
        live += SlotLen(i);
    }
    // Overlapping slots would make Compact() copy more bytes than the page holds.
    if (live > PAGE_SIZE - FreeEnd()) throw DbError("table page is corrupt (overlapping slots)");
}

size_t TablePage::LiveBytes() const {
    size_t total = 0;
    for (uint16_t i = 0; i < NumSlots(); ++i)
        if (SlotOff(i) != 0) total += SlotLen(i);
    return total;
}

size_t TablePage::LiveCount() const {
    size_t n = 0;
    for (uint16_t i = 0; i < NumSlots(); ++i)
        if (SlotOff(i) != 0) ++n;
    return n;
}

std::optional<uint16_t> TablePage::Insert(const char* data, size_t len) {
    CheckIntegrity();
    if (len == 0 || len > MAX_TUPLE) return std::nullopt;
    uint16_t n = NumSlots();
    int slot = -1;
    for (uint16_t i = 0; i < n; ++i) {
        if (SlotOff(i) == 0) {
            slot = i;  // reuse an empty slot
            break;
        }
    }
    size_t need = len + (slot < 0 ? SLOT : 0);
    if (FreeSpace() < need) {
        size_t total_free = PAGE_SIZE - HEADER - n * SLOT - LiveBytes();
        if (total_free < need) return std::nullopt;
        Compact();
    }
    if (slot < 0) {
        slot = n;
        SetNumSlots(static_cast<uint16_t>(n + 1));
    }
    auto off = static_cast<uint16_t>(FreeEnd() - len);
    std::memcpy(d_ + off, data, len);
    SetFreeEnd(off);
    SetSlot(static_cast<uint16_t>(slot), off, static_cast<uint16_t>(len));
    return static_cast<uint16_t>(slot);
}

// Cheap O(1) check for operations that touch a single slot: the header and
// that one slot (if live) must lie inside the page.
void TablePage::CheckSlot(uint16_t slot) const {
    size_t slots_end = HEADER + static_cast<size_t>(NumSlots()) * SLOT;
    if (slots_end > PAGE_SIZE || FreeEnd() < slots_end || FreeEnd() > PAGE_SIZE)
        throw DbError("table page is corrupt (bad header)");
    if (slot >= NumSlots() || SlotOff(slot) == 0) return;
    uint16_t off = SlotOff(slot);
    if (off < FreeEnd() || SlotLen(slot) == 0 || static_cast<size_t>(off) + SlotLen(slot) > PAGE_SIZE)
        throw DbError("table page is corrupt (bad slot " + std::to_string(slot) + ")");
}

bool TablePage::Get(uint16_t slot, std::string* out) const {
    if (HEADER + static_cast<size_t>(NumSlots()) * SLOT > PAGE_SIZE) throw DbError("table page is corrupt (bad header)");
    if (slot >= NumSlots()) return false;
    uint16_t off = SlotOff(slot);
    if (off == 0) return false;
    if (off < HEADER + static_cast<size_t>(NumSlots()) * SLOT || static_cast<size_t>(off) + SlotLen(slot) > PAGE_SIZE)
        throw DbError("table page is corrupt (bad slot " + std::to_string(slot) + ")");
    out->assign(d_ + off, SlotLen(slot));
    return true;
}

bool TablePage::Delete(uint16_t slot) {
    CheckSlot(slot);
    uint16_t n = NumSlots();
    if (slot >= n || SlotOff(slot) == 0) return false;
    SetSlot(slot, 0, 0);
    // trailing empty slots can be dropped entirely
    while (n > 0 && SlotOff(static_cast<uint16_t>(n - 1)) == 0) --n;
    SetNumSlots(n);
    return true;
}

bool TablePage::Update(uint16_t slot, const char* data, size_t len) {
    CheckSlot(slot);
    uint16_t n = NumSlots();
    if (slot >= n || SlotOff(slot) == 0) throw DbError("update of non-existent tuple");
    if (len == 0 || len > MAX_TUPLE) return false;
    uint16_t off = SlotOff(slot), old_len = SlotLen(slot);
    if (len <= old_len) {  // shrink in place
        std::memcpy(d_ + off, data, len);
        SetSlot(slot, off, static_cast<uint16_t>(len));
        return true;
    }
    if (FreeSpace() >= len) {  // write new version into free space
        auto noff = static_cast<uint16_t>(FreeEnd() - len);
        std::memcpy(d_ + noff, data, len);
        SetFreeEnd(noff);
        SetSlot(slot, noff, static_cast<uint16_t>(len));
        return true;
    }
    size_t total_free = PAGE_SIZE - HEADER - n * SLOT - (LiveBytes() - old_len);
    if (total_free < len) return false;
    SetSlot(slot, 0, 0);  // drop old version (slot number is kept), then compact
    Compact();
    auto noff = static_cast<uint16_t>(FreeEnd() - len);
    std::memcpy(d_ + noff, data, len);
    SetFreeEnd(noff);
    SetSlot(slot, noff, static_cast<uint16_t>(len));
    return true;
}

void TablePage::Compact() {
    CheckIntegrity();
    char tmp[PAGE_SIZE];
    size_t end = PAGE_SIZE;
    for (uint16_t i = 0; i < NumSlots(); ++i) {
        uint16_t off = SlotOff(i);
        if (off == 0) continue;
        uint16_t len = SlotLen(i);
        end -= len;
        std::memcpy(tmp + end, d_ + off, len);
        SetSlot(i, static_cast<uint16_t>(end), len);
    }
    std::memcpy(d_ + end, tmp + end, PAGE_SIZE - end);
    SetFreeEnd(static_cast<uint16_t>(end));
}

}  // namespace minidb
