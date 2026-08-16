#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"

#include <gtest/gtest.h>

namespace
{
using fastecu::ErrorKind;
using namespace fastecu::flash;

constexpr std::string_view kNormal = "sub_ecu_hitachi_m32r_kline";
constexpr std::string_view kRecovery = "sub_ecu_hitachi_m32r_kline_recovery";
constexpr std::string_view kMcu = "M32R_512KB_1block";

TEST(SubaruHitachiM32rKlinePlan, MapsExactProtocolsToCompletePortableContract)
{
    for (const auto& [protocol, mode] : {
             std::pair{kNormal, HitachiM32rKlineSessionMode::Normal},
             std::pair{kRecovery, HitachiM32rKlineSessionMode::Recovery},
         })
    {
        for (const auto operation : {FlashOperation::Read, FlashOperation::Write})
        {
            auto plan = build_subaru_hitachi_m32r_kline_plan(
                operation, protocol, kMcu,
                operation == FlashOperation::Write ? std::optional(bytes::Bytes(0x80000, 0x5a)) : std::nullopt);
            ASSERT_TRUE(plan.has_value()) << plan.error().detail;
            EXPECT_EQ(plan->family(), FlashFamily::SubaruHitachiM32rKline);
            EXPECT_EQ(plan->transport(), TransportKind::Kline);
            EXPECT_EQ(plan->transfer_region().start, 0u);
            EXPECT_EQ(plan->transfer_region().length, 0x80000u);
            EXPECT_FALSE(plan->kernel().has_value());
            const auto& family = std::get<SubaruHitachiM32rKlinePlan>(plan->family_plan());
            EXPECT_EQ(family.session_mode, mode);
            EXPECT_EQ(family.tester_id, 0xf0);
            EXPECT_EQ(family.target_id, 0x10);
            EXPECT_EQ(family.initial_baud, 4800);
            EXPECT_EQ(family.write_baud, 15625);
            EXPECT_EQ(family.read_baud, 38400);
            EXPECT_EQ(family.chunk_size, 128u);
            EXPECT_EQ(family.read_address_bias, 0x100000u);
            if (operation == FlashOperation::Write)
            {
                ASSERT_EQ(plan->erase_regions().size(), 1u);
                EXPECT_EQ(plan->erase_regions()[0].start, 0u);
                EXPECT_EQ(plan->erase_regions()[0].length, 0x80000u);
            }
            else
            {
                EXPECT_TRUE(plan->erase_regions().empty());
            }
        }
    }
}

TEST(SubaruHitachiM32rKlinePlan, RejectsInvalidInputsBeforeIo)
{
    const auto expect = [](FlashOperation operation, std::string_view protocol, std::string_view mcu,
                           std::optional<bytes::Bytes> image, ErrorKind expected)
    {
        auto plan = build_subaru_hitachi_m32r_kline_plan(operation, protocol, mcu, std::move(image));
        ASSERT_FALSE(plan.has_value());
        EXPECT_EQ(plan.error().kind, expected);
    };
    expect(FlashOperation::Read, "sub_ecu_hitachi_m32r_kline_typo", kMcu, std::nullopt, ErrorKind::InvalidConfig);
    expect(FlashOperation::Read, kNormal, "M32R_512KB_4blocks", std::nullopt, ErrorKind::InvalidConfig);
    expect(FlashOperation::Write, kNormal, kMcu, std::nullopt, ErrorKind::InvalidConfig);
    expect(FlashOperation::Write, kRecovery, kMcu, bytes::Bytes(0x7ffff), ErrorKind::InvalidConfig);
    expect(FlashOperation::TestWrite, kNormal, kMcu, bytes::Bytes(0x80000), ErrorKind::Unsupported);
}

} // namespace
