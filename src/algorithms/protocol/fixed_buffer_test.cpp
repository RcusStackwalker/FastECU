#include "src/algorithms/protocol/fixed_buffer.h"

#include <array>
#include <string_view>

#include <gtest/gtest.h>

TEST(FixedBufferPortable, ReadsUpToTheTerminator)
{
    const std::array<char, 8> buf{'4', '.', '0', '4', '\0', 'x', 'y', 'z'};
    EXPECT_EQ(bytes::fromFixedBuffer(buf), "4.04");
}

TEST(FixedBufferPortable, StopsAtTheBufferEndWhenNoTerminatorIsPresent)
{
    // The J2534 hazard: PassThruReadVersion takes no length, so a driver may
    // fill every byte and leave nothing for strlen() to stop on.
    const std::array<char, 4> buf{'a', 'b', 'c', 'd'};
    EXPECT_EQ(bytes::fromFixedBuffer(buf), "abcd");
}

TEST(FixedBufferPortable, ReadsEmptyWhenTheFirstByteTerminates)
{
    const std::array<char, 4> buf{};
    EXPECT_TRUE(bytes::fromFixedBuffer(buf).empty());
}

TEST(FixedBufferPortable, ReadsEmptyFromAnEmptyBuffer)
{
    EXPECT_TRUE(bytes::fromFixedBuffer({}).empty());
}

TEST(FixedBufferPortable, DropLastRemovesTheTrailingByte)
{
    const std::array<char, 8> buf{'4', '.', '0', '4', '\r', '\0', 'x', 'y'};
    EXPECT_EQ(bytes::fromFixedBufferDroppingLast(buf), "4.04");
}

TEST(FixedBufferPortable, DropLastStopsAtTheBufferEndWhenNoTerminatorIsPresent)
{
    const std::array<char, 4> buf{'a', 'b', 'c', 'd'};
    EXPECT_EQ(bytes::fromFixedBufferDroppingLast(buf), "abc");
}

TEST(FixedBufferPortable, DropLastLeavesEmptyInputEmpty)
{
    const std::array<char, 4> buf{};
    EXPECT_TRUE(bytes::fromFixedBufferDroppingLast(buf).empty());
    EXPECT_TRUE(bytes::fromFixedBufferDroppingLast({}).empty());
}

TEST(FixedBufferPortable, DropLastOnASingleCharacterYieldsEmpty)
{
    const std::array<char, 4> buf{'x', '\0', '\0', '\0'};
    EXPECT_TRUE(bytes::fromFixedBufferDroppingLast(buf).empty());
}

TEST(FixedBufferPortable, ViewsTheCallerBufferWithoutCopying)
{
    const std::array<char, 8> buf{'1', '.', '2', '\0'};
    EXPECT_EQ(bytes::fromFixedBuffer(buf).data(), buf.data());
}
