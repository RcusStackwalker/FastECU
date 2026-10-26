#include "src/backend/flash/ecu/plan_primitives.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/flash_validation.h"

#include <gmock/gmock.h>
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

TEST(PlanPrimitives, FixedShapeRequiresDeviceTablesAndExactSizeAndBlockCount)
{
    const FlashBlock block{0x100, 0x200};
    FlashDevice device{"test", kSH7058, 0x200, 1, &block, nullptr, &kRegion, nullptr};
    EXPECT_TRUE(FixedShapeDeviceMatches(&device, 0x200, 1));
    EXPECT_FALSE(FixedShapeDeviceMatches(nullptr, 0x200, 1));
    EXPECT_FALSE(FixedShapeDeviceMatches(&device, 0x201, 1));
    EXPECT_FALSE(FixedShapeDeviceMatches(&device, 0x200, 2));
    device.fblocks = nullptr;
    EXPECT_FALSE(FixedShapeDeviceMatches(&device, 0x200, 1));
    device.fblocks = &block;
    device.kblocks = nullptr;
    EXPECT_FALSE(FixedShapeDeviceMatches(&device, 0x200, 1));
}

TEST(PlanPrimitives, TransferRequiresExactFlashStartAndRomLength)
{
    const FlashBlock block{0x100, 0x200};
    const FlashDevice device{"test", kSH7058, 0x200, 1, &block, nullptr, &kRegion, nullptr};
    EXPECT_TRUE(TransferMatchesDevice({0x100, 0x200}, device));
    EXPECT_FALSE(TransferMatchesDevice({0x101, 0x200}, device));
    EXPECT_FALSE(TransferMatchesDevice({0x100, 0x1FF}, device));
}

TEST(PlanPrimitives, RequiresOnlyAcceptsExactlyTheArgumentFreeConfirmation)
{
    using enum ConfirmationSpec::Id;
    const ConfirmationSpec cycle{.id = kCycleIgnition};
    EXPECT_TRUE(RequiresOnly(std::array{cycle}, kCycleIgnition));
    EXPECT_FALSE(RequiresOnly({}, kCycleIgnition));
    EXPECT_FALSE(RequiresOnly(std::array{cycle}, kEraseTrigger));
    EXPECT_FALSE(RequiresOnly(std::array{cycle, cycle}, kCycleIgnition));
    EXPECT_FALSE(RequiresOnly(std::array{ConfirmationSpec{.id = kCycleIgnition, .arguments = {{"key", "value"}}}},
                              kCycleIgnition));
}

TEST(PlanPrimitives, OperationImageShapeRequiresNoReadImageAndExactWriteSize)
{
    for (const FlashOperation operation : {FlashOperation::kRead, FlashOperation::kWrite, FlashOperation::kTestWrite})
    {
        SCOPED_TRACE(::testing::PrintToString(operation));
        auto plan = ValidateAndBuild({
            .operation = operation,
            .family = FlashFamily::kMitsuColtM32rCan,
            .transport = TransportKind::kCanIso15765,
            .target_id = "test",
            .mcu_name = "test",
            .transfer_region = {0, 4},
            .image = operation == FlashOperation::kRead ? std::nullopt : std::optional<bytes::Bytes>{{1, 2, 3, 4}},
            .family_plan = MitsuColtM32rCanPlan{},
        });
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_TRUE(OperationImageShapeMatches(*plan, 4));
        // Mutate a nonconst plan's image solely to exercise defensive checks
        // that ValidateAndBuild normally prevents callers from reaching.
        auto& image = const_cast<std::optional<bytes::Bytes>&>(plan->Image());
        image = bytes::Bytes{1, 2, 3};
        EXPECT_FALSE(OperationImageShapeMatches(*plan, 4));
        image = bytes::Bytes{1, 2, 3, 4, 5};
        EXPECT_FALSE(OperationImageShapeMatches(*plan, 4));
        image = bytes::Bytes{1, 2, 3, 4};
        EXPECT_EQ(OperationImageShapeMatches(*plan, 4), operation != FlashOperation::kRead);
        image.reset();
        EXPECT_EQ(OperationImageShapeMatches(*plan, 4), operation == FlashOperation::kRead);
    }
}

TEST(PlanPrimitives, WholeRomKernelValidationRequiresKernelAndOperationImageShape)
{
    const FlashBlock block{0, 4};
    const FlashDevice device{"test", kSH7058, 4, 1, &block, nullptr, &kRegion, nullptr};
    for (const FlashOperation operation : {FlashOperation::kRead, FlashOperation::kWrite})
    {
        SCOPED_TRACE(::testing::PrintToString(operation));
        auto plan = BuildWholeRomKernelPlan<128>(
            {.operation = operation,
             .family = FlashFamily::kSubaruDensoSh7058CanDiesel,
             .transport = TransportKind::kCanIso15765,
             .target_id = "test",
             .mcu_name = "test",
             .family_plan = SubaruDensoSh7058CanDieselPlan{}},
            operation == FlashOperation::kRead ? std::nullopt : std::optional<bytes::Bytes>{{1, 2, 3, 4}},
            KernelImage{.id = "kernel", .load_address = kRegion.start, .bytes = {1}}, kRegion.start, device,
            [](const FlashPlan&) { return Status{}; });
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_THAT(ValidateWholeRomKernelPlan<128>(*plan, kRegion.start, device), fastecu::testing::IsOk());
        // Mutate the built plan solely to reach checks ValidateAndBuild and the
        // builder normally keep callers from reaching.
        const_cast<std::optional<bytes::Bytes>&>(plan->Image()) = bytes::Bytes{1, 2, 3};
        EXPECT_THAT(ValidateWholeRomKernelPlan<128>(*plan, kRegion.start, device),
                    fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                                ::testing::StrEq(operation == FlashOperation::kRead
                                                                     ? "read plan carries an image"
                                                                     : "ROM file must be exactly 0x4 bytes")));
        const_cast<std::optional<KernelImage>&>(plan->Kernel()).reset();
        EXPECT_THAT(
            ValidateWholeRomKernelPlan<128>(*plan, kRegion.start, device),
            fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig, ::testing::StrEq("plan requires a kernel image")));
    }
}

TEST(PlanPrimitives, AddressMismatchPrecedesPaddingAndRangeErrors)
{
    const auto result = ValidateKernelUpload<128>(std::numeric_limits<std::uint64_t>::max(), 0, 0xFFFF3000, kRegion);
    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                            ::testing::StrEq("kernel address does not match the selected protocol")));
}

TEST(PlanPrimitives, Padded128ByteUploadsMustFitIncludingTheLastBlock)
{
    for (const std::uint64_t size : {1U, 127U, 128U, 129U, 0x8F81U, 0x9000U})
    {
        SCOPED_TRACE(::testing::PrintToString(size));
        EXPECT_THAT((ValidateKernelUpload<128>(size, 0xFFFF3000, 0xFFFF3000, kRegion)), fastecu::testing::IsOk())
            << size;
    }
    const auto over = ValidateKernelUpload<128>(0x9001, 0xFFFF3000, 0xFFFF3000, kRegion);
    EXPECT_THAT(over,
                fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                            ::testing::StrEq("padded kernel lies outside the MCU kernel region")));
    const KernelBlock short_region{0xFFFF3000, 127};
    EXPECT_THAT((ValidateKernelUpload<128>(1, 0xFFFF3000, 0xFFFF3000, short_region)),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(PlanPrimitives, SixBytePaddingUsesItsOwnBoundary)
{
    const KernelBlock region{0x100, 12};
    for (const std::uint64_t size : {1U, 5U, 6U, 7U, 11U, 12U})
    {
        SCOPED_TRACE(::testing::PrintToString(size));
        EXPECT_THAT((ValidateKernelUpload<6>(size, 0x100, 0x100, region)), fastecu::testing::IsOk()) << size;
    }
    EXPECT_THAT((ValidateKernelUpload<6>(13, 0x100, 0x100, region)),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    // An unpadded byte fits, but its six-byte physical transfer does not.
    const KernelBlock short_region{0x100, 5};
    EXPECT_THAT((ValidateKernelUpload<6>(1, 0x100, 0x100, short_region)),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(PlanPrimitives, AddressWindowChecksDoNotUnderflowOrWrapAt32Bits)
{
    for (const std::uint32_t address : {0xFFFF2FFFU, 0xFFFFC000U, 0xFFFFC001U})
    {
        SCOPED_TRACE(::testing::PrintToString(address));
        EXPECT_THAT((ValidateKernelUpload<128>(1, address, address, kRegion)),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig))
            << address;
    }
    // Empty kernels are rejected elsewhere; the range primitive preserves
    // the existing arithmetic boundary at the exclusive region end.
    EXPECT_THAT((ValidateKernelUpload<128>(0, 0xFFFFC000, 0xFFFFC000, kRegion)), fastecu::testing::IsOk());
    const KernelBlock crossing{0xFFFFFFFC, 8};
    EXPECT_THAT((ValidateKernelUpload<6>(6, 0xFFFFFFFC, 0xFFFFFFFC, crossing)), fastecu::testing::IsOk());
    EXPECT_THAT((ValidateKernelUpload<6>(7, 0xFFFFFFFC, 0xFFFFFFFC, crossing)),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

template <std::uint64_t Padding> void ExpectOverflowBoundary()
{
    const std::uint64_t largest_addable = std::numeric_limits<std::uint64_t>::max() - (Padding - 1);
    const auto boundary = ValidateKernelUpload<Padding>(largest_addable, 0xFFFF3000, 0xFFFF3000, kRegion);
    EXPECT_THAT(boundary,
                fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                            ::testing::StrEq("padded kernel lies outside the MCU kernel region")));
    for (const std::uint64_t size : {largest_addable + 1, std::numeric_limits<std::uint64_t>::max()})
    {
        SCOPED_TRACE(::testing::PrintToString(size));
        const auto overflow = ValidateKernelUpload<Padding>(size, 0, 0, kRegion);
        EXPECT_THAT(overflow,
                    fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                                ::testing::StrEq("kernel size cannot be padded to transfer blocks")));
    }
}

TEST(PlanPrimitives, PaddingOverflowPrecedesRangeErrors)
{
    ExpectOverflowBoundary<128>();
    ExpectOverflowBoundary<6>();
}

TEST(PlanPrimitives, EraseRegionsPreserveOrderedBlockAddressesAndLengths)
{
    const std::array<FlashBlock, 3> blocks{{{0, 0x1000}, {0x1000, 0x1000}, {0x2000, 0x6000}}};
    const FlashDevice device{"test", kSH7058, 0x8000, 3, blocks.data(), nullptr, nullptr, nullptr};
    const auto regions = MakeEraseRegions(device);
    ASSERT_EQ(regions.size(), 3U);
    EXPECT_EQ(regions[0].start, 0U);
    EXPECT_EQ(regions[0].length, 0x1000U);
    EXPECT_EQ(regions[1].start, 0x1000U);
    EXPECT_EQ(regions[1].length, 0x1000U);
    EXPECT_EQ(regions[2].start, 0x2000U);
    EXPECT_EQ(regions[2].length, 0x6000U);
    EXPECT_TRUE(EraseGeometryMatches(regions, device));
    for (unsigned index = 0; index < 3; ++index)
    {
        SCOPED_TRACE(::testing::PrintToString(index));
        auto altered = regions;
        ++altered[index].start;
        EXPECT_FALSE(EraseGeometryMatches(altered, device));
        altered = regions;
        --altered[index].length;
        EXPECT_FALSE(EraseGeometryMatches(altered, device));
    }
    EXPECT_FALSE(EraseGeometryMatches(std::vector<MemoryRegion>{}, device));
    auto reordered = regions;
    std::swap(reordered[0], reordered[1]);
    EXPECT_FALSE(EraseGeometryMatches(reordered, device));
}
} // namespace
} // namespace fastecu::flash::detail
