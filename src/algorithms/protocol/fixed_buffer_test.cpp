#include "src/algorithms/protocol/fixed_buffer.h"

#include <array>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::IsEmpty;

TEST(FixedBufferPortable, ReadsUpToTheTerminator)
{
    const std::array<char, 8> buf{'4', '.', '0', '4', '\0', 'x', 'y', 'z'};
    EXPECT_EQ(bytes::FromFixedBuffer(buf), "4.04");
}

TEST(FixedBufferPortable, StopsAtTheBufferEndWhenNoTerminatorIsPresent)
{
    // The J2534 hazard: PassThruReadVersion takes no length, so a driver may
    // fill every byte and leave nothing for strlen() to stop on.
    const std::array<char, 4> buf{'a', 'b', 'c', 'd'};
    EXPECT_EQ(bytes::FromFixedBuffer(buf), "abcd");
}

TEST(FixedBufferPortable, ReadsEmptyWhenTheFirstByteTerminates)
{
    const std::array<char, 4> buf{};
    EXPECT_THAT(bytes::FromFixedBuffer(buf), IsEmpty());
}

TEST(FixedBufferPortable, ReadsEmptyFromAnEmptyBuffer)
{
    EXPECT_THAT(bytes::FromFixedBuffer({}), IsEmpty());
}

TEST(FixedBufferPortable, DropLastRemovesTheTrailingByte)
{
    const std::array<char, 8> buf{'4', '.', '0', '4', '\r', '\0', 'x', 'y'};
    EXPECT_EQ(bytes::FromFixedBufferDroppingLast(buf), "4.04");
}

TEST(FixedBufferPortable, DropLastStopsAtTheBufferEndWhenNoTerminatorIsPresent)
{
    const std::array<char, 4> buf{'a', 'b', 'c', 'd'};
    EXPECT_EQ(bytes::FromFixedBufferDroppingLast(buf), "abc");
}

TEST(FixedBufferPortable, DropLastLeavesEmptyInputEmpty)
{
    const std::array<char, 4> buf{};
    EXPECT_THAT(bytes::FromFixedBufferDroppingLast(buf), IsEmpty());
    EXPECT_THAT(bytes::FromFixedBufferDroppingLast({}), IsEmpty());
}

TEST(FixedBufferPortable, DropLastOnASingleCharacterYieldsEmpty)
{
    const std::array<char, 4> buf{'x', '\0', '\0', '\0'};
    EXPECT_THAT(bytes::FromFixedBufferDroppingLast(buf), IsEmpty());
}

TEST(FixedBufferPortable, ViewsTheCallerBufferWithoutCopying)
{
    const std::array<char, 8> buf{'1', '.', '2', '\0'};
    EXPECT_EQ(bytes::FromFixedBuffer(buf).data(), buf.data());
}
