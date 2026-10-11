#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/backend/flash/flash_validation.h"

#include <gtest/gtest.h>

#include <vector>

namespace fastecu::flash
{
namespace
{
FlashPlanFields ReadFields()
{
    return FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kSubaruHitachiSh7058,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_hitachi_sh7058_can",
        .mcu_name = "SH7058_1block",
        .transfer_region = {0x100000, 0x100000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiSh7058KlinePlan{},
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kStartKlineRead}},
    };
}

FlashPlanFields WriteFields()
{
    return FlashPlanFields{
        .operation = FlashOperation::kWrite,
        .family = FlashFamily::kSubaruHitachiSh7058,
        .transport = TransportKind::kCanIso15765,
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
    auto read = BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                             std::nullopt);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->Transport(), TransportKind::kKline);
    EXPECT_EQ(read->TransferRegion().start, 0x100000U);
    EXPECT_EQ(read->TransferRegion().length, 0x100000U);
    auto write = BuildSubaruHitachiSh7058Plan(FlashOperation::kWrite, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                              bytes::Bytes(0x100000));
    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(write->Transport(), TransportKind::kCanIso15765);
    EXPECT_EQ(write->TransferRegion().start, 0U);
    EXPECT_EQ(write->TransferRegion().length, 0x100000U);
}

TEST(SubaruHitachiSh7058Plan, RejectsUnsafeOperationsAndNearMisses)
{
    EXPECT_FALSE(BuildSubaruHitachiSh7058Plan(FlashOperation::kTestWrite, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                              std::nullopt)
                     .has_value());
    EXPECT_FALSE(BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can_extra",
                                              "SH7058_1block", std::nullopt)
                     .has_value());
    EXPECT_FALSE(
        BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can", "SH7058", std::nullopt)
            .has_value());
    EXPECT_FALSE(BuildSubaruHitachiSh7058Plan(FlashOperation::kWrite, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                              bytes::Bytes(0xFFFFF))
                     .has_value());
}

TEST(SubaruHitachiSh7058Plan, RejectsForgedTransportAndWireParameters)
{
    auto fields = ReadFields();
    fields.family_plan = SubaruHitachiSh7058KlinePlan{.initial_baud = 9600};
    auto forged = ValidateAndBuild(fields);
    ASSERT_TRUE(forged.has_value());
    EXPECT_FALSE(ValidateSubaruHitachiSh7058Plan(*forged).has_value());
    fields.transport = TransportKind::kCanIso15765;
    EXPECT_FALSE(ValidateAndBuild(fields).has_value());
}

TEST(SubaruHitachiSh7058Plan, ReadCarriesTheStartKlineReadConsentAndWriteNone)
{
    auto read = BuildSubaruHitachiSh7058Plan(FlashOperation::kRead, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                             std::nullopt);
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->Confirmations().size(), 1U);
    EXPECT_EQ(read->Confirmations().front().id, ConfirmationSpec::Id::kStartKlineRead);
    EXPECT_TRUE(read->Confirmations().front().arguments.empty());

    auto write = BuildSubaruHitachiSh7058Plan(FlashOperation::kWrite, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                              bytes::Bytes(0x100000));
    ASSERT_TRUE(write.has_value());
    EXPECT_TRUE(write->Confirmations().empty());
}

TEST(SubaruHitachiSh7058Plan, ValidatorAcceptsTheBuilderShapes)
{
    for (auto make : {ReadFields, WriteFields})
    {
        auto plan = ValidateAndBuild(make());
        ASSERT_TRUE(plan.has_value());
        EXPECT_TRUE(ValidateSubaruHitachiSh7058Plan(*plan).has_value());
    }
}

TEST(SubaruHitachiSh7058Plan, ValidatorRequiresExactlyTheReadConsent)
{
    const ConfirmationSpec start_read{.id = ConfirmationSpec::Id::kStartKlineRead};
    struct Case
    {
        const char *name;
        FlashOperation operation;
        std::vector<ConfirmationSpec> confirmations;
    };
    const std::vector<Case> cases{
        {"read without consent", FlashOperation::kRead, {}},
        {"read with another id",
         FlashOperation::kRead,
         {ConfirmationSpec{.id = ConfirmationSpec::Id::kKernelBootstrap}}},
        {"read consent with arguments",
         FlashOperation::kRead,
         {ConfirmationSpec{.id = ConfirmationSpec::Id::kStartKlineRead, .arguments = {{"unexpected", "argument"}}}}},
        {"read with an extra consent",
         FlashOperation::kRead,
         {start_read, ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}}},
        {"write with the read consent", FlashOperation::kWrite, {start_read}},
    };
    for (const Case& test : cases)
    {
        auto fields = test.operation == FlashOperation::kRead ? ReadFields() : WriteFields();
        fields.confirmations = test.confirmations;
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << test.name;
        EXPECT_FALSE(ValidateSubaruHitachiSh7058Plan(*plan).has_value()) << test.name;
    }
}
} // namespace fastecu::flash
