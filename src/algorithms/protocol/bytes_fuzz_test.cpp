#include "src/algorithms/protocol/bytes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace
{
// The runtime-width readers never read outside the buffer: a window that does
// not fit, or a width outside 1-4, reads as 0. Inside the buffer, big- and
// little-endian reads of the same window are byte-reversals of each other.
void ReadsAnyWindowSafely(const std::vector<std::uint8_t>& buffer, std::size_t offset, std::uint32_t width)
{
    const bytes::ByteView view(buffer);
    const std::uint32_t be = bytes::ReadUBe(view, offset, width);
    const std::uint32_t le = bytes::ReadULe(view, offset, width);
    if (width < 1 || width > 4 || offset > buffer.size() || buffer.size() - offset < width)
    {
        EXPECT_EQ(be, 0U);
        EXPECT_EQ(le, 0U);
        return;
    }
    std::uint32_t expected_be = 0;
    std::uint32_t expected_le = 0;
    for (std::uint32_t i = 0; i < width; ++i)
    {
        expected_be = (expected_be << 8) | buffer[offset + i];
        expected_le |= std::uint32_t(buffer[offset + i]) << (8 * i);
    }
    EXPECT_EQ(be, expected_be);
    EXPECT_EQ(le, expected_le);
}
FUZZ_TEST(BytesFuzz, ReadsAnyWindowSafely)
    .WithDomains(fuzztest::VectorOf(fuzztest::Arbitrary<std::uint8_t>()).WithMaxSize(16),
                 fuzztest::InRange<std::size_t>(0, 20), fuzztest::InRange<std::uint32_t>(0, 6));
} // namespace
