#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
using fastecu::ErrorKind;
using namespace fastecu::flash;
using testing::HasSubstr;

constexpr std::string_view kProtocol = "sub_ecu_mitsu_m32r_kline";
constexpr std::string_view kMcu = "M32R_512KB_4blocks";

TEST(SubaruMitsuM32rKlinePlan, SnapshotsExactWireAndMemoryContract)
{
    for (const auto operation : {FlashOperation::Read, FlashOperation::Write})
    {
        auto plan = build_subaru_mitsu_m32r_kline_plan(
            operation, kProtocol, kMcu,
            operation == FlashOperation::Write ? std::optional(bytes::Bytes(0x80000, 0x5a)) : std::nullopt);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        EXPECT_EQ(plan->family(), FlashFamily::SubaruMitsuM32rKline);
        EXPECT_EQ(plan->transport(), TransportKind::Kline);
        EXPECT_EQ(plan->transfer_region().start, 0x8000u);
        EXPECT_EQ(plan->transfer_region().length, 0x78000u);
        EXPECT_FALSE(plan->kernel().has_value());
        const auto& family = std::get<SubaruMitsuM32rKlinePlan>(plan->family_plan());
        EXPECT_EQ(family.tester_id, 0xf0);
        EXPECT_EQ(family.target_id, 0x10);
        EXPECT_EQ(family.initial_baud, 4800);
        EXPECT_EQ(family.flash_baud, 15625);
        EXPECT_EQ(family.chunk_size, 128u);
        EXPECT_EQ(family.unread_prefix_fill, 0xff);
    }
}

TEST(SubaruMitsuM32rKlinePlan, RejectsInvalidInputsBeforeIo)
{
    const auto expect = [](FlashOperation op, std::string_view protocol, std::string_view mcu,
                           std::optional<bytes::Bytes> image, ErrorKind kind)
    {
        auto plan = build_subaru_mitsu_m32r_kline_plan(op, protocol, mcu, std::move(image));
        ASSERT_FALSE(plan.has_value());
        EXPECT_EQ(plan.error().kind, kind);
    };
    expect(FlashOperation::Read, "sub_ecu_mitsu_m32r_kline_typo", kMcu, std::nullopt, ErrorKind::InvalidConfig);
    expect(FlashOperation::Read, kProtocol, "NOT_A_REAL_MCU", std::nullopt, ErrorKind::InvalidConfig);
    expect(FlashOperation::Write, kProtocol, kMcu, std::nullopt, ErrorKind::InvalidConfig);
    expect(FlashOperation::Write, kProtocol, kMcu, bytes::Bytes(0x7ffff), ErrorKind::InvalidConfig);
    expect(FlashOperation::TestWrite, kProtocol, kMcu, bytes::Bytes(0x80000), ErrorKind::Unsupported);
}

} // namespace
