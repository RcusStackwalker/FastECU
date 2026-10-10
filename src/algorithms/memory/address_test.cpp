#include "src/algorithms/memory/address.h"

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>

#include <gtest/gtest.h>

namespace fastecu::memory
{
namespace
{
// A second address space, only to prove that spaces do not mix.
struct RamSpace
{
};

static_assert(!std::is_convertible_v<FlashAddress, FileOffset>);
static_assert(!std::is_convertible_v<FileOffset, FlashAddress>);
static_assert(!std::is_convertible_v<FlashAddress, Address<RamSpace>>);
static_assert(!std::is_convertible_v<std::uint32_t, FlashAddress>);
static_assert(!std::is_convertible_v<FlashAddress, std::uint32_t>);
static_assert(!std::is_convertible_v<std::uint32_t, ByteCount>);
static_assert(!std::equality_comparable_with<FlashAddress, FileOffset>);
static_assert(!std::equality_comparable_with<FlashAddress, Address<RamSpace>>);

AddressRange<FlashSpace> Range(std::uint32_t start, std::uint32_t size)
{
    return AddressRange<FlashSpace>::Make(FlashAddress{start}, ByteCount{size}).value();
}

TEST(TaggedPosition, AdvanceMovesForwardByAByteCount)
{
    EXPECT_EQ(FlashAddress{0x20000}.Advance(ByteCount{0x8000}), FlashAddress{0x28000});
}

TEST(TaggedPosition, AdvanceReachesTheLastAddressButFailsInsteadOfWrapping)
{
    EXPECT_EQ(FlashAddress{0xFFFFFFF0}.Advance(ByteCount{0x0F}), FlashAddress{0xFFFFFFFF});
    EXPECT_EQ(FlashAddress{0xFFFFFFF0}.Advance(ByteCount{0x10}), std::nullopt);
}

TEST(TaggedPosition, DistanceFromCountsTheBytesUpToThisPosition)
{
    EXPECT_EQ(FlashAddress{0x28000}.DistanceFrom(FlashAddress{0x20000}), ByteCount{0x8000});
    EXPECT_EQ(FlashAddress{0x20000}.DistanceFrom(FlashAddress{0x20000}), ByteCount{0});
    EXPECT_EQ(FlashAddress{0x20000}.DistanceFrom(FlashAddress{0x28000}), std::nullopt);
}

TEST(TaggedPosition, FileOffsetsHaveTheSameArithmetic)
{
    EXPECT_EQ(FileOffset{0x20000}.Advance(ByteCount{0x8000}), FileOffset{0x28000});
}

TEST(AddressRange, RejectsEmptyRanges)
{
    EXPECT_EQ(AddressRange<FlashSpace>::Make(FlashAddress{0x100}, ByteCount{0}), std::nullopt);
}

TEST(AddressRange, RejectsRangesWhoseEndIsPastTheAddressSpace)
{
    EXPECT_EQ(AddressRange<FlashSpace>::Make(FlashAddress{0xFFFFFF00}, ByteCount{0x100}), std::nullopt);
    EXPECT_TRUE(AddressRange<FlashSpace>::Make(FlashAddress{0xFFFFFF00}, ByteCount{0xFF}).has_value());
}

TEST(AddressRange, EndIsExclusive)
{
    const auto range = Range(0x20000, 0x8000);

    EXPECT_EQ(range.Start(), FlashAddress{0x20000});
    EXPECT_EQ(range.End(), FlashAddress{0x28000});
    EXPECT_EQ(range.Size(), ByteCount{0x8000});
    EXPECT_TRUE(range.Contains(FlashAddress{0x20000}));
    EXPECT_TRUE(range.Contains(FlashAddress{0x27FFF}));
    EXPECT_FALSE(range.Contains(FlashAddress{0x28000}));
    EXPECT_FALSE(range.Contains(FlashAddress{0x1FFFF}));
}

TEST(AddressRange, ContainsAndOverlapsCompareWholeRanges)
{
    const auto outer = Range(0x0, 0x30000);
    const auto inner = Range(0x20000, 0x8000);
    const auto adjacent = Range(0x28000, 0x10);

    EXPECT_TRUE(outer.Contains(inner));
    EXPECT_FALSE(inner.Contains(outer));
    EXPECT_TRUE(outer.Overlaps(inner));
    EXPECT_TRUE(inner.Overlaps(outer));
    EXPECT_FALSE(inner.Overlaps(adjacent));
    EXPECT_FALSE(adjacent.Overlaps(inner));
}
} // namespace
} // namespace fastecu::memory
