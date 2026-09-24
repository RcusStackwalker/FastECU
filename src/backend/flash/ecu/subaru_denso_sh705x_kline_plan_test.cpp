#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"

#include <array>
#include <string_view>
#include <tuple>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
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
    bool cobb;
};

constexpr auto kPairs = std::to_array<Pair>({
    {"sub_ecu_denso_sh7055_04", "SH7055", SubaruDensoSh705xKlineSeedKey::Stock, false},
    {"sub_ecu_denso_sh7055_04_ecutek", "SH7055", SubaruDensoSh705xKlineSeedKey::EcuTek, false},
    {"sub_ecu_denso_sh7055_04_cobb", "SH7055", SubaruDensoSh705xKlineSeedKey::Stock, true},
    {"sub_ecu_denso_sh7058", "SH7058", SubaruDensoSh705xKlineSeedKey::Stock, false},
    {"sub_ecu_denso_sh7058_ecutek", "SH7058", SubaruDensoSh705xKlineSeedKey::EcuTek, false},
    {"sub_ecu_denso_sh7058_cobb", "SH7058", SubaruDensoSh705xKlineSeedKey::Stock, true},
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

} // namespace
} // namespace fastecu::flash
