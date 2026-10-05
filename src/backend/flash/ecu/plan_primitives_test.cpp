#include "src/backend/flash/ecu/plan_primitives.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace fastecu::flash::detail
{
namespace
{
constexpr kernelblock kRegion{0xFFFF3000, 0x9000};

TEST(PlanPrimitives, Padded128ByteUploadsMustFitIncludingTheLastBlock)
{
    for (const std::uint64_t size : {1U, 127U, 128U, 129U, 0x8F81U, 0x9000U})
    {
        EXPECT_TRUE((validate_padded_kernel_range<std::uint64_t, 128>(size, 0xFFFF3000, kRegion, "overflow", "outside"))
                        .has_value())
            << size;
    }
    const auto over =
        validate_padded_kernel_range<std::uint64_t, 128>(0x9001, 0xFFFF3000, kRegion, "overflow", "outside");
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().detail, "outside");
    const kernelblock short_region{0xFFFF3000, 127};
    EXPECT_FALSE((validate_padded_kernel_range<std::uint64_t, 128>(1, 0xFFFF3000, short_region, "overflow", "outside"))
                     .has_value());
}

TEST(PlanPrimitives, SixBytePaddingUsesItsOwnBoundary)
{
    const kernelblock region{0x100, 12};
    for (const std::uint64_t size : {1U, 5U, 6U, 7U, 11U, 12U})
    {
        EXPECT_TRUE(
            (validate_padded_kernel_range<std::uint64_t, 6>(size, 0x100, region, "overflow", "outside")).has_value())
            << size;
    }
    EXPECT_FALSE(
        (validate_padded_kernel_range<std::uint64_t, 6>(13, 0x100, region, "overflow", "outside")).has_value());
    // An unpadded byte fits, but its six-byte physical transfer does not.
    const kernelblock short_region{0x100, 5};
    EXPECT_FALSE(
        (validate_padded_kernel_range<std::uint64_t, 6>(1, 0x100, short_region, "overflow", "outside")).has_value());
}

TEST(PlanPrimitives, AddressWindowChecksDoNotUnderflowOrWrapAt32Bits)
{
    for (const std::uint32_t address : {0xFFFF2FFFU, 0xFFFFC000U, 0xFFFFC001U})
    {
        EXPECT_FALSE(
            (validate_padded_kernel_range<std::uint64_t, 128>(1, address, kRegion, "overflow", "outside")).has_value())
            << address;
    }
    // Empty kernels are rejected elsewhere; the range primitive preserves
    // the existing arithmetic boundary at the exclusive region end.
    EXPECT_TRUE(
        (validate_padded_kernel_range<std::uint64_t, 128>(0, 0xFFFFC000, kRegion, "overflow", "outside")).has_value());
    const kernelblock crossing{0xFFFFFFFC, 8};
    EXPECT_TRUE(
        (validate_padded_kernel_range<std::uint64_t, 6>(6, 0xFFFFFFFC, crossing, "overflow", "outside")).has_value());
    EXPECT_FALSE(
        (validate_padded_kernel_range<std::uint64_t, 6>(7, 0xFFFFFFFC, crossing, "overflow", "outside")).has_value());
}

template <typename Size, Size Padding> void expect_overflow_boundary()
{
    const Size largest_addable = std::numeric_limits<Size>::max() - (Padding - 1);
    const auto boundary =
        validate_padded_kernel_range<Size, Padding>(largest_addable, 0xFFFF3000, kRegion, "overflow", "outside");
    ASSERT_FALSE(boundary.has_value());
    EXPECT_EQ(boundary.error().detail, "outside");
    for (const Size size : {static_cast<Size>(largest_addable + 1), std::numeric_limits<Size>::max()})
    {
        const auto overflow = validate_padded_kernel_range<Size, Padding>(size, 0, kRegion, "overflow", "outside");
        ASSERT_FALSE(overflow.has_value());
        EXPECT_EQ(overflow.error(), (Error{ErrorKind::InvalidConfig, "overflow"}));
    }
}

TEST(PlanPrimitives, PaddingOverflowPreservesSizeTypeAndPrecedesRangeErrors)
{
    expect_overflow_boundary<std::uint32_t, 128>();
    expect_overflow_boundary<std::uint64_t, 128>();
    expect_overflow_boundary<std::uint64_t, 6>();
}

TEST(PlanPrimitives, EraseRegionsPreserveOrderedBlockAddressesAndLengths)
{
    const std::array<flashblock, 3> blocks{{{0, 0x1000}, {0x1000, 0x1000}, {0x2000, 0x6000}}};
    const flashdev_t device{"test", SH7058, 0x8000, 3, blocks.data(), nullptr, nullptr, nullptr};
    const auto regions = make_erase_regions(device);
    ASSERT_EQ(regions.size(), 3U);
    EXPECT_EQ(regions[0].start, 0U);
    EXPECT_EQ(regions[0].length, 0x1000U);
    EXPECT_EQ(regions[1].start, 0x1000U);
    EXPECT_EQ(regions[1].length, 0x1000U);
    EXPECT_EQ(regions[2].start, 0x2000U);
    EXPECT_EQ(regions[2].length, 0x6000U);
    EXPECT_TRUE(erase_geometry_matches(regions, device));
    for (unsigned index = 0; index < 3; ++index)
    {
        auto altered = regions;
        ++altered[index].start;
        EXPECT_FALSE(erase_geometry_matches(altered, device));
        altered = regions;
        --altered[index].length;
        EXPECT_FALSE(erase_geometry_matches(altered, device));
    }
    EXPECT_FALSE(erase_geometry_matches(std::vector<MemoryRegion>{}, device));
    auto reordered = regions;
    std::swap(reordered[0], reordered[1]);
    EXPECT_FALSE(erase_geometry_matches(reordered, device));
}
} // namespace
} // namespace fastecu::flash::detail
