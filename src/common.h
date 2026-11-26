// common.h - basic types shared by every layer of MiniDB.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

namespace minidb {

// Every page on disk and in memory has exactly this size.
constexpr size_t PAGE_SIZE = 4096;

using page_id_t = int32_t;   // index of a page inside the database file
using frame_id_t = int32_t;  // index of a frame inside the buffer pool

constexpr page_id_t INVALID_PAGE_ID = -1;
constexpr page_id_t CATALOG_PAGE_ID = 0;  // page 0 always holds the system catalog

// Record ID: physical address of a tuple = (page, slot inside the page).
struct RID {
    page_id_t page_id = INVALID_PAGE_ID;
    uint32_t slot = 0;

    bool operator==(const RID& o) const { return page_id == o.page_id && slot == o.slot; }
    bool operator!=(const RID& o) const { return !(*this == o); }
    bool operator<(const RID& o) const {
        return page_id != o.page_id ? page_id < o.page_id : slot < o.slot;
    }
};

// All user-visible errors (syntax errors, constraint violations, ...) use this type.
class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Helpers for reading / writing fixed-size values at arbitrary (unaligned) offsets
// inside a raw page buffer.
template <typename T>
inline T ReadAs(const char* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <typename T>
inline void WriteAs(char* p, T v) {
    std::memcpy(p, &v, sizeof(T));
}

}  // namespace minidb
