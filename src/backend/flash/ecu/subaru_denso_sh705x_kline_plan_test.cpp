#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan_detail.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

KernelImage KernelFor(std::string_view mcu)
{
    return KernelImage{
        .id = "kernel", .load_address = mcu == "SH7055" ? 0xFFFF6004U : 0xFFFF3000U, .bytes = {0xAA, 0xBB, 0xCC, 0xDD}};
}

std::uint32_t Romsize(std::string_view mcu)
{
    return FindFlashDevice(mcu)->romsize;
}

bytes::Bytes ImageFor(std::string_view mcu)
{
    return bytes::Bytes(Romsize(mcu), 0xFF);
}

struct Pair
{
    std::string_view protocol;
    std::string_view mcu;
    SubaruDensoSh705xKlineSeedKey seed_key;
};

constexpr auto kPairs = std::to_array<Pair>({
    {"sub_ecu_denso_sh7055_04", "SH7055", SubaruDensoSh705xKlineSeedKey::kStock},
    {"sub_ecu_denso_sh7055_04_ecutek", "SH7055", SubaruDensoSh705xKlineSeedKey::kEcuTek},
    {"sub_ecu_denso_sh7055_04_cobb", "SH7055", SubaruDensoSh705xKlineSeedKey::kStock},
    {"sub_ecu_denso_sh7058", "SH7058", SubaruDensoSh705xKlineSeedKey::kStock},
    {"sub_ecu_denso_sh7058_ecutek", "SH7058", SubaruDensoSh705xKlineSeedKey::kEcuTek},
    {"sub_ecu_denso_sh7058_cobb", "SH7058", SubaruDensoSh705xKlineSeedKey::kStock},
});

TEST(SubaruDensoSh705xKlinePlan, MapsAllSixPairsWithLegacyWireParameters)
{
    for (const Pair& pair : kPairs)
    {
        SCOPED_TRACE(pair.protocol);
        const auto plan = BuildSubaruDensoSh705xKlinePlan(FlashOperation::kTestWrite, pair.protocol, pair.mcu,
                                                          ImageFor(pair.mcu), KernelFor(pair.mcu));
        ASSERT_THAT(plan, IsOk());
        EXPECT_EQ(plan->Family(), FlashFamily::kSubaruDensoSh705xKline);
        EXPECT_EQ(plan->Transport(), TransportKind::kKline);
        EXPECT_EQ(plan->TransferRegion(), (MemoryRegion{0, Romsize(pair.mcu)}));
        EXPECT_TRUE(plan->EraseRegions().empty());
        EXPECT_TRUE(plan->Confirmations().empty());
        const auto& family = std::get<SubaruDensoSh705xKlinePlan>(plan->FamilyPlan());
        // execute():67-76 -- 4800 baud, tester 0xF0, target 0x10.
        EXPECT_EQ(family.initial_baud, 4800);
        EXPECT_EQ(family.tester_id, 0xF0);
        EXPECT_EQ(family.target_id, 0x10);
        // connect_bootloader():269 -- flash method endsWith("_ecutek").
        EXPECT_EQ(family.seed_key, pair.seed_key);
    }
}

TEST(SubaruDensoSh705xKlinePlan, ReadCarriesNoImage)
{
    const auto plan = BuildSubaruDensoSh705xKlinePlan(FlashOperation::kRead, "sub_ecu_denso_sh7058", "SH7058",
                                                      ImageFor("SH7058"), KernelFor("SH7058"));
    ASSERT_THAT(plan, IsOk());
    EXPECT_FALSE(plan->Image().has_value());
}

TEST(SubaruDensoSh705xKlinePlan, CobbIsTestWriteOnly)
{
    for (const std::string_view protocol : {"sub_ecu_denso_sh7055_04_cobb", "sub_ecu_denso_sh7058_cobb"})
    {
        SCOPED_TRACE(protocol);
        const std::string_view mcu = protocol.find("sh7055") != std::string_view::npos ? "SH7055" : "SH7058";
        EXPECT_THAT(BuildSubaruDensoSh705xKlinePlan(FlashOperation::kRead, protocol, mcu, std::nullopt, KernelFor(mcu)),
                    IsErr(ErrorKind::kUnsupported));
        EXPECT_THAT(
            BuildSubaruDensoSh705xKlinePlan(FlashOperation::kWrite, protocol, mcu, ImageFor(mcu), KernelFor(mcu)),
            IsErr(ErrorKind::kUnsupported));
    }
}

TEST(SubaruDensoSh705xKlinePlan, RejectsUnknownCrossPairedAndLookalikeIdentities)
{
    const auto rejected = std::to_array<std::pair<std::string_view, std::string_view>>({
        {"sub_ecu_denso_sh7055_04", "SH7058"},
        {"sub_ecu_denso_sh7058", "SH7055"},
        {"sub_ecu_denso_sh7055_04_future", "SH7055"},
        {"sub_ecu_denso_sh7058_can", "SH7058"},
        {"sub_ecu_denso_sh7055_02", "SH7055"},
        {"sub_ecu_denso_sh7058", "SH7058_1block"},
    });
    for (const auto& [protocol, mcu] : rejected)
    {
        SCOPED_TRACE(protocol);
        EXPECT_THAT(
            BuildSubaruDensoSh705xKlinePlan(FlashOperation::kRead, protocol, mcu, std::nullopt, KernelFor("SH7058")),
            IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruDensoSh705xKlinePlan, RejectsBadKernels)
{
    KernelImage empty = KernelFor("SH7055");
    empty.bytes.clear();
    EXPECT_THAT(BuildSubaruDensoSh705xKlinePlan(FlashOperation::kRead, "sub_ecu_denso_sh7055_04", "SH7055",
                                                std::nullopt, empty),
                IsErr(ErrorKind::kInvalidConfig));

    // SH7058's kernel address on an SH7055 protocol.
    EXPECT_THAT(BuildSubaruDensoSh705xKlinePlan(FlashOperation::kRead, "sub_ecu_denso_sh7055_04", "SH7055",
                                                std::nullopt, KernelFor("SH7058")),
                IsErr(ErrorKind::kInvalidConfig));

    KernelImage oversized = KernelFor("SH7058");
    oversized.bytes.assign(0x01000000, 0x00);
    EXPECT_THAT(BuildSubaruDensoSh705xKlinePlan(FlashOperation::kRead, "sub_ecu_denso_sh7058", "SH7058", std::nullopt,
                                                oversized),
                IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoSh705xKlinePlan, WritesRequireAnExactRomSizedImage)
{
    for (const FlashOperation operation : {FlashOperation::kWrite, FlashOperation::kTestWrite})
    {
        EXPECT_THAT(BuildSubaruDensoSh705xKlinePlan(operation, "sub_ecu_denso_sh7058", "SH7058", std::nullopt,
                                                    KernelFor("SH7058")),
                    IsErr(ErrorKind::kInvalidConfig));
        EXPECT_THAT(BuildSubaruDensoSh705xKlinePlan(operation, "sub_ecu_denso_sh7058", "SH7058",
                                                    bytes::Bytes(Romsize("SH7058") - 1, 0xFF), KernelFor("SH7058")),
                    IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruDensoSh705xKlinePlan, DeviceGeometrySatisfiesTheExecutorsChunking)
{
    // The executor writes 0x200-byte chunks and commits 0x1000-byte blocks,
    // indexing the image by physical address; the plan relies on this.
    for (const std::string_view mcu : {"SH7055", "SH7058"})
    {
        const FlashDevice *device = FindFlashDevice(mcu);
        ASSERT_NE(device, nullptr);
        EXPECT_EQ(device->fblocks[0].start, 0U);
        std::uint32_t total = 0;
        for (unsigned i = 0; i < device->numblocks; ++i)
        {
            EXPECT_EQ(device->fblocks[i].len % 0x1000, 0U);
            total += device->fblocks[i].len;
        }
        EXPECT_EQ(total, device->romsize);
    }
}

// ---- validate_subaru_denso_sh705x_kline_plan() on hand-built plans --------

// The fields build_subaru_denso_sh705x_kline_plan() produces for a
// sub_ecu_denso_sh7055_04 TestWrite; each case below breaks exactly one.
FlashPlanFields ValidFields(FlashOperation operation = FlashOperation::kTestWrite,
                            std::string target_id = "sub_ecu_denso_sh7055_04")
{
    return FlashPlanFields{
        .operation = operation,
        .family = FlashFamily::kSubaruDensoSh705xKline,
        .transport = TransportKind::kKline,
        .target_id = std::move(target_id),
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{0, Romsize("SH7055")},
        .erase_regions = {},
        .image = operation == FlashOperation::kRead ? std::nullopt : std::optional<bytes::Bytes>(ImageFor("SH7055")),
        .kernel = KernelFor("SH7055"),
        .family_plan = SubaruDensoSh705xKlinePlan{.initial_baud = 4800,
                                                  .tester_id = 0xF0,
                                                  .target_id = 0x10,
                                                  .seed_key = SubaruDensoSh705xKlineSeedKey::kStock},
        .confirmations = {},
    };
}

SubaruDensoSh705xKlinePlan& FamilyOf(FlashPlanFields& fields)
{
    return std::get<SubaruDensoSh705xKlinePlan>(fields.family_plan);
}

TEST(SubaruDensoSh705xKlinePlan, ValidatorAcceptsTheUnmodifiedHandBuiltPlan)
{
    auto plan = ValidateAndBuild(ValidFields());
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(ValidateSubaruDensoSh705xKlinePlan(*plan), IsOk());
}

TEST(SubaruDensoSh705xKlinePlan, ValidatorRejectsEachSingleBadField)
{
    const std::vector<std::pair<std::string_view, std::function<void(FlashPlanFields&)>>> cases{
        {"foreign family",
         [](FlashPlanFields& f)
         {
             f.family = FlashFamily::kSubaruUnisiaJecs;
             f.target_id = "sub_ecu_unisia_jecs_m3779x";
             f.family_plan = SubaruUnisiaJecsPlan{.initial_baud = 1953, .even_parity = true};
         }},
        {"unknown protocol", [](FlashPlanFields& f) { f.target_id = "sub_ecu_denso_sh7055_02"; }},
        {"wrong MCU for the protocol", [](FlashPlanFields& f) { f.mcu_name = "SH7058"; }},
        {"baud", [](FlashPlanFields& f) { FamilyOf(f).initial_baud = 9600; }},
        {"tester id", [](FlashPlanFields& f) { FamilyOf(f).tester_id = 0xF1; }},
        {"target id", [](FlashPlanFields& f) { FamilyOf(f).target_id = 0x11; }},
        {"seed variant", [](FlashPlanFields& f) { FamilyOf(f).seed_key = SubaruDensoSh705xKlineSeedKey::kEcuTek; }},
        {"erase regions", [](FlashPlanFields& f) { f.erase_regions = {MemoryRegion{0, 0x1000}}; }},
        {"confirmations",
         [](FlashPlanFields& f) { f.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}}; }},
        {"kernel address", [](FlashPlanFields& f) { f.kernel->load_address = 0xFFFF3000U; }},
        {"transfer region start", [](FlashPlanFields& f) { f.transfer_region = {0x1000, Romsize("SH7055")}; }},
        {"transfer region length", [](FlashPlanFields& f) { f.transfer_region = {0, Romsize("SH7055") - 0x1000}; }},
        {"image size", [](FlashPlanFields& f) { f.image = bytes::Bytes(Romsize("SH7055") - 1, 0xFF); }},
    };
    for (const auto& [name, mutate] : cases)
    {
        SCOPED_TRACE(name);
        FlashPlanFields fields = ValidFields();
        mutate(fields);
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_THAT(plan, IsOk());
        EXPECT_THAT(ValidateSubaruDensoSh705xKlinePlan(*plan), IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruDensoSh705xKlinePlan, ValidatorRejectsAHandBuiltCobbRead)
{
    auto plan = ValidateAndBuild(ValidFields(FlashOperation::kRead, "sub_ecu_denso_sh7055_04_cobb"));
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(ValidateSubaruDensoSh705xKlinePlan(*plan), IsErr(ErrorKind::kUnsupported));
}

TEST(SubaruDensoSh705xKlinePlan, GeometryCheckRejectsSyntheticTablesTheExecutorCannotChunk)
{
    using detail::ValidateSubaruDensoSh705xKlineGeometry;
    const auto aligned = std::to_array<FlashBlock>({{0x0000, 0x1000}, {0x1000, 0x1000}});
    const auto offset = std::to_array<FlashBlock>({{0x1000, 0x1000}});
    const auto ragged = std::to_array<FlashBlock>({{0x0000, 0x1000}, {0x1000, 0x0200}});

    const auto device = [](std::uint32_t size, unsigned count, const FlashBlock *blocks)
    {
        return FlashDevice{.name = "synthetic",
                           .mcutype = FindFlashDevice("SH7055")->mcutype,
                           .romsize = size,
                           .numblocks = count,
                           .fblocks = blocks,
                           .rblocks = nullptr,
                           .kblocks = nullptr,
                           .eblocks = nullptr};
    };
    EXPECT_THAT(ValidateSubaruDensoSh705xKlineGeometry(device(0x2000, 2, aligned.data())), IsOk());
    EXPECT_THAT(ValidateSubaruDensoSh705xKlineGeometry(device(0x2000, 0, aligned.data())),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(ValidateSubaruDensoSh705xKlineGeometry(device(0x1000, 1, offset.data())),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(ValidateSubaruDensoSh705xKlineGeometry(device(0x1200, 2, ragged.data())),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(ValidateSubaruDensoSh705xKlineGeometry(device(0x3000, 2, aligned.data())),
                IsErr(ErrorKind::kInvalidConfig));
}

} // namespace
} // namespace fastecu::flash
