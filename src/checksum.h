// checksum.h - CRC-32C, used to detect damaged pages in the database file.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "common.h"

namespace minidb {

namespace detail {

// Lookup table for CRC-32C (Castagnoli, reflected polynomial 0x82F63B78),
// built at compile time.
constexpr std::array<uint32_t, 256> MakeCrc32cTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        table[i] = c;
    }
    return table;
}

inline constexpr std::array<uint32_t, 256> kCrc32cTable = MakeCrc32cTable();

}  // namespace detail

// CRC-32C of `len` bytes. Pass the previous result as `crc` to checksum data
// in several pieces: Crc32c(b, nb, Crc32c(a, na)) == Crc32c(a ++ b).
inline uint32_t Crc32c(const void* data, size_t len, uint32_t crc = 0) {
    const auto* p = static_cast<const unsigned char*>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) crc = detail::kCrc32cTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// The checksum stored next to a page on disk. It covers the page id as well as
// the contents, so an intact page found at the wrong place in the file (a
// misdirected write) is reported as damaged too.
inline uint32_t PageChecksum(page_id_t pid, const char* data) {
    return Crc32c(data, PAGE_SIZE, Crc32c(&pid, sizeof(pid)));
}

}  // namespace minidb
