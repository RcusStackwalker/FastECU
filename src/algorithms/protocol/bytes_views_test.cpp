#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/bytes_views.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <ranges>
#include <vector>

using ::testing::ElementsAre;

using bytes::views::kU32Be;

namespace
{

template <typename Range> std::vector<std::uint32_t> Words(Range&& range)
{
    return {std::ranges::begin(range), std::ranges::end(range)};
}

} // namespace

TEST(U32BeView, ReadsEachFourBytesAsOneBigEndianWord)
{
    const std::array<bytes::Byte, 8> data{0x01, 0x02, 0x03, 0x04, 0xA0, 0xB0, 0xC0, 0xD0};
    EXPECT_THAT(Words(data | kU32Be), ElementsAre(0x01020304U, 0xA0B0C0D0U));
}

TEST(U32BeView, IsEmptyForEmptyInput)
{
    EXPECT_THAT(Words(std::span<const bytes::Byte>{} | kU32Be), ElementsAre());
}

TEST(U32BeView, IgnoresATrailingPartialWord)
{
    const std::array<bytes::Byte, 7> data{0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB, 0xCC};
    EXPECT_THAT(Words(data | kU32Be), ElementsAre(0x01020304U));
}

TEST(U32BeView, IsEmptyWhenShorterThanOneWord)
{
    const std::array<bytes::Byte, 3> data{0x01, 0x02, 0x03};
    EXPECT_THAT(Words(data | kU32Be), ElementsAre());
}

TEST(U32BeView, KnowsItsSizeAndIndexesRandomly)
{
    const std::array<bytes::Byte, 12> data{
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
    };
    const auto words = data | kU32Be;
    EXPECT_EQ(std::ranges::size(words), 3U);
    EXPECT_EQ(words[2], 3U);
    EXPECT_EQ(words[0], 1U);
}

TEST(U32BeView, IsCallableAsWellAsPipeable)
{
    const std::array<bytes::Byte, 4> data{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_THAT(Words(kU32Be(data)), ElementsAre(0xDEADBEEFU));
}

TEST(U32BeView, ComposesWithComposeBeAsTheInverse)
{
    const std::array<bytes::Byte, 8> data{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    EXPECT_THAT(bytes::ComposeBe(data | kU32Be), test_bytes::BytesEq(bytes::Bytes(data.begin(), data.end())));
}
