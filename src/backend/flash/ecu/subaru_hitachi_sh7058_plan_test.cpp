#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/backend/flash/flash_validation.h"

#include <gtest/gtest.h>

#include <vector>

namespace fastecu::flash
{
namespace
{
FlashPlanFields read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruHitachiSh7058,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_hitachi_sh7058_can",
        .mcu_name = "SH7058_1block",
        .transfer_region = {0x100000, 0x100000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiSh7058KlinePlan{},
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::StartKlineRead}},
    };
}

FlashPlanFields write_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Write,
        .family = FlashFamily::SubaruHitachiSh7058,
        .transport = TransportKind::CanIso15765,
        .target_id = "sub_ecu_hitachi_sh7058_can",
        .mcu_name = "SH7058_1block",
        .transfer_region = {0, 0x100000},
        .erase_regions = {{0, 0x100000}},
        .image = bytes::Bytes(0x100000),
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiSh7058CanPlan{},
        .confirmations = {},
    };
}
} // namespace

TEST(SubaruHitachiSh7058Plan, ReadAndWriteHaveDistinctTransportAndExactGeometry)
{
    auto read = build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 std::nullopt);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->transport(), TransportKind::Kline);
    EXPECT_EQ(read->transfer_region().start, 0x100000U);
    EXPECT_EQ(read->transfer_region().length, 0x100000U);
    auto write = build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                  bytes::Bytes(0x100000));
    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(write->transport(), TransportKind::CanIso15765);
    EXPECT_EQ(write->transfer_region().start, 0U);
    EXPECT_EQ(write->transfer_region().length, 0x100000U);
}

TEST(SubaruHitachiSh7058Plan, RejectsUnsafeOperationsAndNearMisses)
{
    EXPECT_FALSE(build_subaru_hitachi_sh7058_plan(FlashOperation::TestWrite, "sub_ecu_hitachi_sh7058_can",
                                                  "SH7058_1block", std::nullopt)
                     .has_value());
    EXPECT_FALSE(build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can_extra",
                                                  "SH7058_1block", std::nullopt)
                     .has_value());
    EXPECT_FALSE(
        build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can", "SH7058", std::nullopt)
            .has_value());
    EXPECT_FALSE(build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                  bytes::Bytes(0xFFFFF))
                     .has_value());
}

TEST(SubaruHitachiSh7058Plan, RejectsForgedTransportAndWireParameters)
{
    auto fields = read_fields();
    fields.family_plan = SubaruHitachiSh7058KlinePlan{.initial_baud = 9600};
    auto forged = validate_and_build(fields);
    ASSERT_TRUE(forged.has_value());
    EXPECT_FALSE(validate_subaru_hitachi_sh7058_plan(*forged).has_value());
    fields.transport = TransportKind::CanIso15765;
    EXPECT_FALSE(validate_and_build(fields).has_value());
}

TEST(SubaruHitachiSh7058Plan, ReadCarriesTheStartKlineReadConsentAndWriteNone)
{
    auto read = build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 std::nullopt);
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->confirmations().size(), 1U);
    EXPECT_EQ(read->confirmations().front().id, ConfirmationSpec::Id::StartKlineRead);
    EXPECT_TRUE(read->confirmations().front().arguments.empty());

    auto write = build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                  bytes::Bytes(0x100000));
    ASSERT_TRUE(write.has_value());
    EXPECT_TRUE(write->confirmations().empty());
}

TEST(SubaruHitachiSh7058Plan, ValidatorAcceptsTheBuilderShapes)
{
    for (auto make : {read_fields, write_fields})
    {
        auto plan = validate_and_build(make());
        ASSERT_TRUE(plan.has_value());
        EXPECT_TRUE(validate_subaru_hitachi_sh7058_plan(*plan).has_value());
    }
}

TEST(SubaruHitachiSh7058Plan, ValidatorRequiresExactlyTheReadConsent)
{
    const ConfirmationSpec start_read{.id = ConfirmationSpec::Id::StartKlineRead};
    struct Case
    {
        const char *name;
        FlashOperation operation;
        std::vector<ConfirmationSpec> confirmations;
    };
    const std::vector<Case> cases{
        {"read without consent", FlashOperation::Read, {}},
        {"read with another id", FlashOperation::Read, {ConfirmationSpec{.id = ConfirmationSpec::Id::KernelBootstrap}}},
        {"read consent with arguments",
         FlashOperation::Read,
         {ConfirmationSpec{.id = ConfirmationSpec::Id::StartKlineRead, .arguments = {{"unexpected", "argument"}}}}},
        {"read with an extra consent",
         FlashOperation::Read,
         {start_read, ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}}},
        {"write with the read consent", FlashOperation::Write, {start_read}},
    };
    for (const Case& test : cases)
    {
        auto fields = test.operation == FlashOperation::Read ? read_fields() : write_fields();
        fields.confirmations = test.confirmations;
        auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << test.name;
        EXPECT_FALSE(validate_subaru_hitachi_sh7058_plan(*plan).has_value()) << test.name;
    }
}
} // namespace fastecu::flash
