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

#include "src/backend/definitions/kernelmemorymodels.h"
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

KernelImage kernel_for(std::string_view mcu)
{
    return KernelImage{
        .id = "kernel", .load_address = mcu == "SH7055" ? 0xFFFF6004U : 0xFFFF3000U, .bytes = {0xAA, 0xBB, 0xCC, 0xDD}};
}

std::uint32_t romsize(std::string_view mcu)
{
    return find_flash_device(mcu)->romsize;
}

bytes::Bytes image_for(std::string_view mcu)
{
    return bytes::Bytes(romsize(mcu), 0xFF);
}

struct Pair
{
    std::string_view protocol;
    std::string_view mcu;
    SubaruDensoSh705xKlineSeedKey seed_key;
};

constexpr auto kPairs = std::to_array<Pair>({
    {"sub_ecu_denso_sh7055_04", "SH7055", SubaruDensoSh705xKlineSeedKey::Stock},
    {"sub_ecu_denso_sh7055_04_ecutek", "SH7055", SubaruDensoSh705xKlineSeedKey::EcuTek},
    {"sub_ecu_denso_sh7055_04_cobb", "SH7055", SubaruDensoSh705xKlineSeedKey::Stock},
    {"sub_ecu_denso_sh7058", "SH7058", SubaruDensoSh705xKlineSeedKey::Stock},
    {"sub_ecu_denso_sh7058_ecutek", "SH7058", SubaruDensoSh705xKlineSeedKey::EcuTek},
    {"sub_ecu_denso_sh7058_cobb", "SH7058", SubaruDensoSh705xKlineSeedKey::Stock},
});

TEST(SubaruDensoSh705xKlinePlan, MapsAllSixPairsWithLegacyWireParameters)
{
    for (const Pair& pair : kPairs)
    {
        SCOPED_TRACE(pair.protocol);
        const auto plan = build_subaru_denso_sh705x_kline_plan(FlashOperation::TestWrite, pair.protocol, pair.mcu,
                                                               image_for(pair.mcu), kernel_for(pair.mcu));
        ASSERT_THAT(plan, IsOk());
        EXPECT_EQ(plan->family(), FlashFamily::SubaruDensoSh705xKline);
        EXPECT_EQ(plan->transport(), TransportKind::Kline);
        EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0, romsize(pair.mcu)}));
        EXPECT_TRUE(plan->erase_regions().empty());
        EXPECT_TRUE(plan->confirmations().empty());
        const auto& family = std::get<SubaruDensoSh705xKlinePlan>(plan->family_plan());
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
    const auto plan = build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, "sub_ecu_denso_sh7058", "SH7058",
                                                           image_for("SH7058"), kernel_for("SH7058"));
    ASSERT_THAT(plan, IsOk());
    EXPECT_FALSE(plan->image().has_value());
}

TEST(SubaruDensoSh705xKlinePlan, CobbIsTestWriteOnly)
{
    for (const std::string_view protocol : {"sub_ecu_denso_sh7055_04_cobb", "sub_ecu_denso_sh7058_cobb"})
    {
        SCOPED_TRACE(protocol);
        const std::string_view mcu = protocol.find("sh7055") != std::string_view::npos ? "SH7055" : "SH7058";
        EXPECT_THAT(
            build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, protocol, mcu, std::nullopt, kernel_for(mcu)),
            IsErr(ErrorKind::Unsupported));
        EXPECT_THAT(
            build_subaru_denso_sh705x_kline_plan(FlashOperation::Write, protocol, mcu, image_for(mcu), kernel_for(mcu)),
            IsErr(ErrorKind::Unsupported));
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
        EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, protocol, mcu, std::nullopt,
                                                         kernel_for("SH7058")),
                    IsErr(ErrorKind::InvalidConfig));
    }
}

TEST(SubaruDensoSh705xKlinePlan, RejectsBadKernels)
{
    KernelImage empty = kernel_for("SH7055");
    empty.bytes.clear();
    EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, "sub_ecu_denso_sh7055_04", "SH7055",
                                                     std::nullopt, empty),
                IsErr(ErrorKind::InvalidConfig));

    // SH7058's kernel address on an SH7055 protocol.
    EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, "sub_ecu_denso_sh7055_04", "SH7055",
                                                     std::nullopt, kernel_for("SH7058")),
                IsErr(ErrorKind::InvalidConfig));

    KernelImage oversized = kernel_for("SH7058");
    oversized.bytes.assign(0x01000000, 0x00);
    EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, "sub_ecu_denso_sh7058", "SH7058",
                                                     std::nullopt, oversized),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruDensoSh705xKlinePlan, WritesRequireAnExactRomSizedImage)
{
    for (const FlashOperation operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(operation, "sub_ecu_denso_sh7058", "SH7058", std::nullopt,
                                                         kernel_for("SH7058")),
                    IsErr(ErrorKind::InvalidConfig));
        EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(operation, "sub_ecu_denso_sh7058", "SH7058",
                                                         bytes::Bytes(romsize("SH7058") - 1, 0xFF),
                                                         kernel_for("SH7058")),
                    IsErr(ErrorKind::InvalidConfig));
    }
}

TEST(SubaruDensoSh705xKlinePlan, DeviceGeometrySatisfiesTheExecutorsChunking)
{
    // The executor writes 0x200-byte chunks and commits 0x1000-byte blocks,
    // indexing the image by physical address; the plan relies on this.
    for (const std::string_view mcu : {"SH7055", "SH7058"})
    {
        const flashdev_t *device = find_flash_device(mcu);
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
FlashPlanFields valid_fields(FlashOperation operation = FlashOperation::TestWrite,
                             std::string target_id = "sub_ecu_denso_sh7055_04")
{
    return FlashPlanFields{
        .operation = operation,
        .family = FlashFamily::SubaruDensoSh705xKline,
        .transport = TransportKind::Kline,
        .target_id = std::move(target_id),
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{0, romsize("SH7055")},
        .erase_regions = {},
        .image = operation == FlashOperation::Read ? std::nullopt : std::optional<bytes::Bytes>(image_for("SH7055")),
        .kernel = kernel_for("SH7055"),
        .family_plan = SubaruDensoSh705xKlinePlan{.initial_baud = 4800,
                                                  .tester_id = 0xF0,
                                                  .target_id = 0x10,
                                                  .seed_key = SubaruDensoSh705xKlineSeedKey::Stock},
        .confirmations = {},
    };
}

SubaruDensoSh705xKlinePlan& family_of(FlashPlanFields& fields)
{
    return std::get<SubaruDensoSh705xKlinePlan>(fields.family_plan);
}

TEST(SubaruDensoSh705xKlinePlan, ValidatorAcceptsTheUnmodifiedHandBuiltPlan)
{
    auto plan = validate_and_build(valid_fields());
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(validate_subaru_denso_sh705x_kline_plan(*plan), IsOk());
}

TEST(SubaruDensoSh705xKlinePlan, ValidatorRejectsEachSingleBadField)
{
    const std::vector<std::pair<std::string_view, std::function<void(FlashPlanFields&)>>> cases{
        {"foreign family",
         [](FlashPlanFields& f)
         {
             f.family = FlashFamily::SubaruUnisiaJecs;
             f.target_id = "sub_ecu_unisia_jecs_m3779x";
             f.family_plan = SubaruUnisiaJecsPlan{.initial_baud = 1953, .even_parity = true};
         }},
        {"unknown protocol", [](FlashPlanFields& f) { f.target_id = "sub_ecu_denso_sh7055_02"; }},
        {"wrong MCU for the protocol", [](FlashPlanFields& f) { f.mcu_name = "SH7058"; }},
        {"baud", [](FlashPlanFields& f) { family_of(f).initial_baud = 9600; }},
        {"tester id", [](FlashPlanFields& f) { family_of(f).tester_id = 0xF1; }},
        {"target id", [](FlashPlanFields& f) { family_of(f).target_id = 0x11; }},
        {"seed variant", [](FlashPlanFields& f) { family_of(f).seed_key = SubaruDensoSh705xKlineSeedKey::EcuTek; }},
        {"erase regions", [](FlashPlanFields& f) { f.erase_regions = {MemoryRegion{0, 0x1000}}; }},
        {"confirmations",
         [](FlashPlanFields& f) { f.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}}; }},
        {"kernel address", [](FlashPlanFields& f) { f.kernel->load_address = 0xFFFF3000U; }},
        {"transfer region start", [](FlashPlanFields& f) { f.transfer_region = {0x1000, romsize("SH7055")}; }},
        {"transfer region length", [](FlashPlanFields& f) { f.transfer_region = {0, romsize("SH7055") - 0x1000}; }},
        {"image size", [](FlashPlanFields& f) { f.image = bytes::Bytes(romsize("SH7055") - 1, 0xFF); }},
    };
    for (const auto& [name, mutate] : cases)
    {
        SCOPED_TRACE(name);
        FlashPlanFields fields = valid_fields();
        mutate(fields);
        auto plan = validate_and_build(std::move(fields));
        ASSERT_THAT(plan, IsOk());
        EXPECT_THAT(validate_subaru_denso_sh705x_kline_plan(*plan), IsErr(ErrorKind::InvalidConfig));
    }
}

TEST(SubaruDensoSh705xKlinePlan, ValidatorRejectsAHandBuiltCobbRead)
{
    auto plan = validate_and_build(valid_fields(FlashOperation::Read, "sub_ecu_denso_sh7055_04_cobb"));
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(validate_subaru_denso_sh705x_kline_plan(*plan), IsErr(ErrorKind::Unsupported));
}

TEST(SubaruDensoSh705xKlinePlan, GeometryCheckRejectsSyntheticTablesTheExecutorCannotChunk)
{
    using detail::validate_subaru_denso_sh705x_kline_geometry;
    const auto aligned = std::to_array<flashblock>({{0x0000, 0x1000}, {0x1000, 0x1000}});
    const auto offset = std::to_array<flashblock>({{0x1000, 0x1000}});
    const auto ragged = std::to_array<flashblock>({{0x0000, 0x1000}, {0x1000, 0x0200}});

    const auto device = [](std::uint32_t size, unsigned count, const flashblock *blocks)
    {
        return flashdev_t{.name = "synthetic",
                          .mcutype = find_flash_device("SH7055")->mcutype,
                          .romsize = size,
                          .numblocks = count,
                          .fblocks = blocks,
                          .rblocks = nullptr,
                          .kblocks = nullptr,
                          .eblocks = nullptr};
    };
    EXPECT_THAT(validate_subaru_denso_sh705x_kline_geometry(device(0x2000, 2, aligned.data())), IsOk());
    EXPECT_THAT(validate_subaru_denso_sh705x_kline_geometry(device(0x2000, 0, aligned.data())),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(validate_subaru_denso_sh705x_kline_geometry(device(0x1000, 1, offset.data())),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(validate_subaru_denso_sh705x_kline_geometry(device(0x1200, 2, ragged.data())),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(validate_subaru_denso_sh705x_kline_geometry(device(0x3000, 2, aligned.data())),
                IsErr(ErrorKind::InvalidConfig));
}

} // namespace
} // namespace fastecu::flash
