// Unit tests: CRC-32C and the per-page checksum built on it.
#include <string>

#include "checksum.h"
#include "test_util.h"

namespace minidb {
namespace {

TEST(Checksum, Crc32cKnownVectors) {
    EXPECT_EQ(Crc32c("", 0), 0u);
    EXPECT_EQ(Crc32c("123456789", 9), 0xE3069283u);  // the standard CRC-32C check value
    // Test vectors from RFC 3720 (iSCSI), appendix B.4.
    std::string zeros(32, '\0'), ones(32, '\xFF');
    EXPECT_EQ(Crc32c(zeros.data(), zeros.size()), 0x8A9136AAu);
    EXPECT_EQ(Crc32c(ones.data(), ones.size()), 0x62A8AB43u);
}

TEST(Checksum, IncrementalEqualsOneShot) {
    std::string s = "The quick brown fox jumps over the lazy dog";
    for (size_t split = 0; split <= s.size(); ++split) {
        uint32_t first = Crc32c(s.data(), split);
        EXPECT_EQ(Crc32c(s.data() + split, s.size() - split, first), Crc32c(s.data(), s.size())) << split;
    }
}

TEST(Checksum, PageChecksumDetectsEverySingleBitFlip) {
    char page[PAGE_SIZE];
    for (size_t i = 0; i < PAGE_SIZE; ++i) page[i] = static_cast<char>(i * 31 + 7);
    const uint32_t good = PageChecksum(5, page);
    for (size_t byte = 0; byte < PAGE_SIZE; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            page[byte] = static_cast<char>(page[byte] ^ (1 << bit));
            ASSERT_NE(PageChecksum(5, page), good) << "byte " << byte << " bit " << bit;
            page[byte] = static_cast<char>(page[byte] ^ (1 << bit));
        }
    }
    EXPECT_EQ(PageChecksum(5, page), good);
}

TEST(Checksum, PageChecksumDependsOnPageId) {
    char page[PAGE_SIZE] = {};
    EXPECT_NE(PageChecksum(1, page), PageChecksum(2, page));
    EXPECT_NE(PageChecksum(0, page), 0u) << "an all-zero frame on disk must not look valid";
}

}  // namespace
}  // namespace minidb
