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

// Checked 64-bit integer arithmetic. Signed overflow is undefined behaviour in
// C++ (and INT64_MIN / -1 traps on x86, killing the process), so every INT
// operation reachable from SQL goes through these and reports an SQL error.
namespace checked {

constexpr int64_t kMin = INT64_MIN;
constexpr int64_t kMax = INT64_MAX;

[[noreturn]] inline void OutOfRange() { throw DbError("integer out of range"); }

inline int64_t Add(int64_t a, int64_t b) {
    if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) OutOfRange();
    return a + b;
}
inline int64_t Sub(int64_t a, int64_t b) {
    if ((b < 0 && a > kMax + b) || (b > 0 && a < kMin + b)) OutOfRange();
    return a - b;
}
inline int64_t Mul(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    bool overflow = a > 0 ? (b > 0 ? a > kMax / b : b < kMin / a) : (b > 0 ? a < kMin / b : b < kMax / a);
    if (overflow) OutOfRange();
    return a * b;
}
// Callers check b != 0 first (that is a "division by zero" error, not a range error).
inline int64_t Div(int64_t a, int64_t b) {
    if (a == kMin && b == -1) OutOfRange();
    return a / b;
}
inline int64_t Mod(int64_t a, int64_t b) {
    if (b == -1) return 0;  // INT64_MIN % -1 traps on x86, although the result is 0
    return a % b;
}
inline int64_t Neg(int64_t a) {
    if (a == kMin) OutOfRange();
    return -a;
}
// DOUBLE -> INT conversion (truncates toward zero). NaN, infinities and values
// outside the INT range are errors instead of undefined behaviour.
inline int64_t FromDouble(double d) {
    // +-2^63 are exactly representable as doubles; INT64_MAX is not. Written as
    // a negated comparison so that NaN fails it too.
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) OutOfRange();
    return static_cast<int64_t>(d);
}

}  // namespace checked

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
