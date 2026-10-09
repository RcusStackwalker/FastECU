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
constexpr KernelBlock kRegion{0xFFFF3000, 0x9000};

TEST(PlanPrimitives, AddressMismatchPrecedesPaddingAndRangeErrors)
{
    const auto result = validate_kernel_upload<128>(std::numeric_limits<std::uint64_t>::max(), 0, 0xFFFF3000, kRegion);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(),
              (Error{ErrorKind::kInvalidConfig, "kernel address does not match the selected protocol"}));
}

TEST(PlanPrimitives, Padded128ByteUploadsMustFitIncludingTheLastBlock)
{
    for (const std::uint64_t size : {1U, 127U, 128U, 129U, 0x8F81U, 0x9000U})
    {
        EXPECT_TRUE((validate_kernel_upload<128>(size, 0xFFFF3000, 0xFFFF3000, kRegion)).has_value()) << size;
    }
    const auto over = validate_kernel_upload<128>(0x9001, 0xFFFF3000, 0xFFFF3000, kRegion);
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().detail, "padded kernel lies outside the MCU kernel region");
    const KernelBlock short_region{0xFFFF3000, 127};
    EXPECT_FALSE((validate_kernel_upload<128>(1, 0xFFFF3000, 0xFFFF3000, short_region)).has_value());
}

TEST(PlanPrimitives, SixBytePaddingUsesItsOwnBoundary)
{
    const KernelBlock region{0x100, 12};
    for (const std::uint64_t size : {1U, 5U, 6U, 7U, 11U, 12U})
    {
        EXPECT_TRUE((validate_kernel_upload<6>(size, 0x100, 0x100, region)).has_value()) << size;
    }
    EXPECT_FALSE((validate_kernel_upload<6>(13, 0x100, 0x100, region)).has_value());
    // An unpadded byte fits, but its six-byte physical transfer does not.
    const KernelBlock short_region{0x100, 5};
    EXPECT_FALSE((validate_kernel_upload<6>(1, 0x100, 0x100, short_region)).has_value());
}

TEST(PlanPrimitives, AddressWindowChecksDoNotUnderflowOrWrapAt32Bits)
{
    for (const std::uint32_t address : {0xFFFF2FFFU, 0xFFFFC000U, 0xFFFFC001U})
    {
        EXPECT_FALSE((validate_kernel_upload<128>(1, address, address, kRegion)).has_value()) << address;
    }
    // Empty kernels are rejected elsewhere; the range primitive preserves
    // the existing arithmetic boundary at the exclusive region end.
    EXPECT_TRUE((validate_kernel_upload<128>(0, 0xFFFFC000, 0xFFFFC000, kRegion)).has_value());
    const KernelBlock crossing{0xFFFFFFFC, 8};
    EXPECT_TRUE((validate_kernel_upload<6>(6, 0xFFFFFFFC, 0xFFFFFFFC, crossing)).has_value());
    EXPECT_FALSE((validate_kernel_upload<6>(7, 0xFFFFFFFC, 0xFFFFFFFC, crossing)).has_value());
}

template <std::uint64_t Padding> void expect_overflow_boundary()
{
    const std::uint64_t largest_addable = std::numeric_limits<std::uint64_t>::max() - (Padding - 1);
    const auto boundary = validate_kernel_upload<Padding>(largest_addable, 0xFFFF3000, 0xFFFF3000, kRegion);
    ASSERT_FALSE(boundary.has_value());
    EXPECT_EQ(boundary.error().detail, "padded kernel lies outside the MCU kernel region");
    for (const std::uint64_t size : {largest_addable + 1, std::numeric_limits<std::uint64_t>::max()})
    {
        const auto overflow = validate_kernel_upload<Padding>(size, 0, 0, kRegion);
        ASSERT_FALSE(overflow.has_value());
        EXPECT_EQ(overflow.error(),
                  (Error{ErrorKind::kInvalidConfig, "kernel size cannot be padded to transfer blocks"}));
    }
}

TEST(PlanPrimitives, PaddingOverflowPrecedesRangeErrors)
{
    expect_overflow_boundary<128>();
    expect_overflow_boundary<6>();
}

TEST(PlanPrimitives, EraseRegionsPreserveOrderedBlockAddressesAndLengths)
{
    const std::array<FlashBlock, 3> blocks{{{0, 0x1000}, {0x1000, 0x1000}, {0x2000, 0x6000}}};
    const FlashDevice device{"test", kSH7058, 0x8000, 3, blocks.data(), nullptr, nullptr, nullptr};
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
