#include "src/backend/flash/ecu/testing/n83m_payloads.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <set>

#include "src/algorithms/protocol/testing/byte_matchers.h"

namespace fastecu::flash::testing
{
namespace
{
TEST(N83mPayloads, KnownAnswerSamples)
{
    const auto forward = MakeN83mPayload(0x184000, N83mPattern::kForward);
    const auto reverse = MakeN83mPayload(0x3E4000, N83mPattern::kReverse);
    EXPECT_EQ(forward.plain.size(), 0x184000U);
    EXPECT_EQ(forward.wire.size(), 0x184000U);
    EXPECT_EQ(reverse.plain.size(), 0x3E4000U);
    EXPECT_EQ(reverse.wire.size(), 0x3E4000U);
    const std::array plain{bytes::Bytes{0, 1, 2, 3}, bytes::Bytes{0xFC, 0xFD, 0xFE, 0xFF},
                           bytes::Bytes{0, 0x11, 0x22, 0x33}, bytes::Bytes{0x44, 0x55, 0x66, 0x77}};
    const std::array wire{bytes::Bytes{0xE0, 0xD3, 0x85, 0x2B}, bytes::Bytes{0xC5, 0xFE, 0x4B, 0x39},
                          bytes::Bytes{0xED, 0x9F, 0xD9, 0x31}, bytes::Bytes{0xAF, 0xAC, 0xD5, 0x94}};
    for (std::size_t page = 0; page < 4; ++page)
    {
        SCOPED_TRACE(page);
        EXPECT_THAT(bytes::ByteView(forward.plain).subspan(page * 256, 4), test_bytes::BytesEq(plain[page]));
        EXPECT_THAT(bytes::ByteView(forward.wire).subspan(page * 256, 4), test_bytes::BytesEq(wire[page]));
    }
    EXPECT_THAT(bytes::ByteView(forward.plain).subspan(4, 4), test_bytes::BytesEq(plain[0]));
    EXPECT_THAT(bytes::ByteView(forward.plain).subspan(0x10010, 4), test_bytes::BytesEq(plain[1]));
    EXPECT_THAT(bytes::ByteView(forward.wire).subspan(0x10010, 4), test_bytes::BytesEq(wire[1]));
    EXPECT_THAT(bytes::ByteView(reverse.plain).first(4), test_bytes::BytesEq(plain[3]));
    EXPECT_THAT(bytes::ByteView(reverse.wire).first(4), test_bytes::BytesEq(wire[3]));
    EXPECT_THAT(bytes::ByteView(reverse.plain).subspan(256 + 15 * 4, 4), test_bytes::BytesEq(plain[2]));
    EXPECT_THAT(bytes::ByteView(reverse.wire).subspan(256 + 15 * 4, 4), test_bytes::BytesEq(wire[2]));
}

TEST(N83mPayloads, EveryPageHasDistinctIdentity)
{
    for (const std::size_t size : {0x184000U, 0x3E4000U})
    {
        SCOPED_TRACE(size);
        for (const auto pattern : {N83mPattern::kForward, N83mPattern::kReverse})
        {
            SCOPED_TRACE(static_cast<int>(pattern));
            const auto payload = MakeN83mPayload(size, pattern);
            for (const auto *image : {&payload.plain, &payload.wire})
            {
                std::set<bytes::Bytes> pages;
                for (std::size_t offset = 0; offset < size; offset += 256)
                {
                    pages.emplace(image->begin() + static_cast<std::ptrdiff_t>(offset),
                                  image->begin() + static_cast<std::ptrdiff_t>(offset + 256));
                }
                EXPECT_EQ(pages.size(), size / 256);
            }
        }
    }
}

TEST(N83mPayloads, PatternsAreDistinctAndRepeatable)
{
    for (const std::size_t size : {0x184000U, 0x3E4000U})
    {
        SCOPED_TRACE(size);
        const auto forward = MakeN83mPayload(size, N83mPattern::kForward);
        const auto repeat = MakeN83mPayload(size, N83mPattern::kForward);
        const auto reverse = MakeN83mPayload(size, N83mPattern::kReverse);
        EXPECT_THAT(repeat.plain, test_bytes::BytesEq(forward.plain));
        EXPECT_THAT(repeat.wire, test_bytes::BytesEq(forward.wire));
        for (std::size_t offset = 0; offset < size; offset += 256)
        {
            SCOPED_TRACE(offset);
            EXPECT_THAT(bytes::ByteView(reverse.plain).subspan(offset, 256),
                        ::testing::Not(test_bytes::BytesEq(bytes::ByteView(forward.plain).subspan(offset, 256))));
            EXPECT_THAT(bytes::ByteView(reverse.wire).subspan(offset, 256),
                        ::testing::Not(test_bytes::BytesEq(bytes::ByteView(forward.wire).subspan(offset, 256))));
        }
    }
}
} // namespace
} // namespace fastecu::flash::testing
