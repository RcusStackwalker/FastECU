#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_types.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_types.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_types.h"
#include "src/backend/flash/flash_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash
{
namespace
{

TEST(TransportConfigProjectionTest, CopiesIso15765WireFields)
{
    constexpr SubaruHitachiM32rCanPlan kPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };

    constexpr Iso15765Config kConfig = Iso15765ConfigFrom(kPlan);

    EXPECT_EQ(kConfig.bitrate, 500000);
    EXPECT_EQ(kConfig.request_id, 0x7e0U);
    EXPECT_EQ(kConfig.response_id, 0x7e8U);
    EXPECT_FALSE(kConfig.extended_id);
}

TEST(TransportConfigProjectionTest, CopiesNonIso14230KlineWireFields)
{
    constexpr SubaruMitsuM32rKlinePlan kPlan{
        .tester_id = 0xf0,
        .target_id = 0x10,
        .initial_baud = 4800,
        .flash_baud = 62500,
        .chunk_size = 0x80,
        .unread_prefix_fill = 0x00,
    };

    constexpr KlineConfig kConfig = NonIso14230KlineConfigFrom(kPlan);

    EXPECT_EQ(kConfig.baud, 4800);
    EXPECT_FALSE(kConfig.iso14230);
    EXPECT_EQ(kConfig.tester_id, 0xf0);
    EXPECT_EQ(kConfig.target_id, 0x10);
}

FlashPlanFields KlineReadFields()
{
    return FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kDensoSh705xEepromKline,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{.start = 0xf000, .length = 0x1000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xffff2000, .bytes = {0x01}},
        .family_plan =
            DensoSh705xEepromKlinePlan{
                .mode = EepromReadMode::kMode2,
                .security = DensoSecurityVariant::kStock,
                .tester_id = 0xf0,
                .target_id = 0x10,
                .initial_baud = 4800,
                .kernel_baud = 15625,
            },
        .confirmations =
            {
                ConfirmationSpec{.id = ConfirmationSpec::Id::kBeginEepromRead},
                ConfirmationSpec{.id = ConfirmationSpec::Id::kInspectEepromBytes},
            },
    };
}

TEST(CheckFamilyTest, MatchingFamilyPasses)
{
    auto plan = ValidateAndBuild(KlineReadFields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    EXPECT_THAT(CheckFamily(*plan, FlashFamily::kDensoSh705xEepromKline), fastecu::testing::IsOk());
}

TEST(CheckFamilyTest, WrongFamilyFailsWithInvalidConfig)
{
    auto plan = ValidateAndBuild(KlineReadFields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ASSERT_THAT(CheckFamily(*plan, FlashFamily::kMitsuColtM32rCan), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

FlashPlanFields ForeignCanReadFields()
{
    return FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kDensoSh705xEepromCan,
        .transport = TransportKind::kCanIso15765,
        .target_id = "sub_ecu_denso_sh705x_eeprom_can",
        .mcu_name = "SH7058",
        .transfer_region = MemoryRegion{.start = 0x0, .length = 0x100},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xffff6004, .bytes = {0x01, 0x02}},
        .family_plan =
            DensoSh705xEepromCanPlan{
                .mode = EepromReadMode::kMode2,
                .security = DensoSecurityVariant::kStock,
                .request_id = 0x7e0,
                .response_id = 0x7e8,
                .bitrate = 500000,
                .extended_id = false,
            },
        .confirmations = {},
    };
}

// Stands in for a family validator so each admission test chooses whether
// validation passes, and can tell whether it ran at all.
struct CountingValidator
{
    static inline int calls_ = 0;
    static inline Status result_ = {};

    static Status Validate(const FlashPlan& /*plan*/)
    {
        ++calls_;
        return result_;
    }
};

class ProbeExecutor final : public FamilyExecutor<DensoSh705xEepromKlinePlan, IKlineFlashExecutor, CountingValidator>
{
  public:
    mutable const DensoSh705xEepromKlinePlan *setup_family = nullptr;
    const DensoSh705xEepromKlinePlan *run_family = nullptr;
    const FlashPlan *run_plan = nullptr;

  private:
    Result<KlineConfig> Setup(const DensoSh705xEepromKlinePlan& family) const override
    {
        setup_family = &family;
        return NonIso14230KlineConfigFrom(family);
    }

    Result<FlashExecutionResult> Run(const DensoSh705xEepromKlinePlan& family, const FlashPlan& plan,
                                     IKlineFlashTransport& /*transport*/, IClock& /*clock*/,
                                     const ICancellationToken& /*cancellation*/, IEventSink& /*events*/) override
    {
        run_family = &family;
        run_plan = &plan;
        return FlashExecutionResult{.operation = plan.Operation(), .read_bytes = std::nullopt};
    }
};

class CanProbeExecutor final : public FamilyExecutor<DensoSh705xEepromCanPlan, ICanFlashExecutor, CountingValidator>
{
  public:
    const FlashPlan *run_plan = nullptr;

  private:
    Result<Iso15765Config> Setup(const DensoSh705xEepromCanPlan& family) const override
    {
        return Iso15765ConfigFrom(family);
    }

    Result<FlashExecutionResult> Run(const DensoSh705xEepromCanPlan& /*family*/, const FlashPlan& plan,
                                     ICanFlashTransport& /*transport*/, IClock& /*clock*/,
                                     const ICancellationToken& /*cancellation*/, IEventSink& /*events*/) override
    {
        run_plan = &plan;
        return FlashExecutionResult{.operation = plan.Operation(), .read_bytes = std::nullopt};
    }
};

class FamilyExecutorTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        CountingValidator::calls_ = 0;
        CountingValidator::result_ = {};
    }

    Result<FlashExecutionResult> Execute(const FlashPlan& plan)
    {
        return executor_.Execute(plan, transport_, clock_, cancellation_, events_);
    }

    ProbeExecutor executor_;
    ScriptedKlineFlashTransport transport_;
    FakeClock clock_;
    FakeCancellationToken cancellation_;
    RecordingEventSink events_;
};

TEST_F(FamilyExecutorTest, TransportSetupRejectsForeignFamily)
{
    auto foreign = ValidateAndBuild(ForeignCanReadFields());
    ASSERT_THAT(foreign, fastecu::testing::IsOk());

    const auto config = executor_.TransportSetup(*foreign);

    ASSERT_THAT(config, fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                                    ::testing::Eq("plan family does not match this executor")));
    EXPECT_EQ(CountingValidator::calls_, 0);
    EXPECT_EQ(executor_.setup_family, nullptr);
}

TEST_F(FamilyExecutorTest, ForeignFamilyNeverReachesValidator)
{
    auto foreign = ValidateAndBuild(ForeignCanReadFields());
    ASSERT_THAT(foreign, fastecu::testing::IsOk());

    const auto result = Execute(*foreign);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                                    ::testing::Eq("plan family does not match this executor")));
    EXPECT_EQ(CountingValidator::calls_, 0);
    EXPECT_EQ(executor_.run_plan, nullptr);
    EXPECT_EQ(transport_.WritesConsumed(), 0U);
}

TEST_F(FamilyExecutorTest, TransportSetupReturnsValidatorError)
{
    auto plan = ValidateAndBuild(KlineReadFields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    CountingValidator::result_ = Fail(ErrorKind::kInvalidConfig, "probe invalid");

    const auto config = executor_.TransportSetup(*plan);

    ASSERT_THAT(config, fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig, ::testing::Eq("probe invalid")));
    EXPECT_EQ(CountingValidator::calls_, 1);
    EXPECT_EQ(executor_.setup_family, nullptr);
}

TEST_F(FamilyExecutorTest, TransportSetupHandsFamilyPlanToSetup)
{
    auto plan = ValidateAndBuild(KlineReadFields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto config = executor_.TransportSetup(*plan);

    ASSERT_THAT(config, fastecu::testing::IsOk());
    EXPECT_EQ(executor_.setup_family, &plan->FamilyPlan().Get<DensoSh705xEepromKlinePlan>());
    EXPECT_EQ(config->baud, 4800);
    EXPECT_EQ(config->target_id, 0x10);
}

TEST_F(FamilyExecutorTest, ExecuteValidatesBeforeCheckingCancellation)
{
    auto plan = ValidateAndBuild(KlineReadFields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    CountingValidator::result_ = Fail(ErrorKind::kInvalidConfig, "probe invalid");
    cancellation_.SetCancelled(true);

    const auto result = Execute(*plan);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig, ::testing::Eq("probe invalid")));
    EXPECT_EQ(cancellation_.CheckCount(), 0U);
}

TEST_F(FamilyExecutorTest, ExecuteCancelledAfterAdmissionChecks)
{
    auto plan = ValidateAndBuild(KlineReadFields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    cancellation_.SetCancelled(true);

    const auto result = Execute(*plan);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before setup")));
    EXPECT_EQ(CountingValidator::calls_, 1);
    EXPECT_EQ(executor_.run_plan, nullptr);
    EXPECT_EQ(transport_.WritesConsumed(), 0U);
    EXPECT_THAT(events_.logs, ::testing::IsEmpty());
}

TEST_F(FamilyExecutorTest, ExecuteHandsFamilyAndFlashPlanToRun)
{
    auto plan = ValidateAndBuild(KlineReadFields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = Execute(*plan);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kRead);
    EXPECT_EQ(executor_.run_family, &plan->FamilyPlan().Get<DensoSh705xEepromKlinePlan>());
    EXPECT_EQ(executor_.run_plan, &*plan);
}

TEST_F(FamilyExecutorTest, AdmissionReachedThroughInterface)
{
    auto plan = ValidateAndBuild(KlineReadFields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    IKlineFlashExecutor& through_interface = executor_;

    EXPECT_THAT(through_interface.TransportSetup(*plan), fastecu::testing::IsOk());
    EXPECT_THAT(through_interface.Execute(*plan, transport_, clock_, cancellation_, events_), fastecu::testing::IsOk());
    EXPECT_EQ(CountingValidator::calls_, 2);
}

TEST_F(FamilyExecutorTest, CanExecuteRefusesForeignFamilyBeforeValidator)
{
    auto foreign = ValidateAndBuild(KlineReadFields());
    ASSERT_THAT(foreign, fastecu::testing::IsOk());
    CanProbeExecutor can_executor;
    ScriptedCanFlashTransport can_transport;

    const auto result = can_executor.Execute(*foreign, can_transport, clock_, cancellation_, events_);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                                    ::testing::Eq("plan family does not match this executor")));
    EXPECT_EQ(CountingValidator::calls_, 0);
    EXPECT_EQ(can_executor.run_plan, nullptr);
}

TEST_F(FamilyExecutorTest, CanExecuteCancelledAfterAdmissionChecks)
{
    auto plan = ValidateAndBuild(ForeignCanReadFields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    CanProbeExecutor can_executor;
    ScriptedCanFlashTransport can_transport;
    cancellation_.SetCancelled(true);

    const auto result = can_executor.Execute(*plan, can_transport, clock_, cancellation_, events_);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before setup")));
    EXPECT_EQ(CountingValidator::calls_, 1);
    EXPECT_EQ(can_executor.run_plan, nullptr);
    EXPECT_THAT(events_.logs, ::testing::IsEmpty());
}

// A hand-written executor: implements the interface directly, so nothing runs
// plan admission for it.
class UnadmittedExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> TransportSetup(const FlashPlan& /*plan*/) const override
    {
        return KlineConfig{};
    }

    Result<FlashExecutionResult> Execute(const FlashPlan& plan, IKlineFlashTransport& /*transport*/, IClock& /*clock*/,
                                         const ICancellationToken& /*cancellation*/, IEventSink& /*events*/) override
    {
        return FlashExecutionResult{.operation = plan.Operation(), .read_bytes = std::nullopt};
    }
};

TEST(AdmittingExecutorTest, RejectsAnExecutorThatSkipsFamilyExecutor)
{
    static_assert(!AdmittingExecutor<UnadmittedExecutor>);
}

TEST(AdmittingExecutorTest, RejectsAFamilyExecutorWithACustomValidator)
{
    static_assert(!AdmittingExecutor<ProbeExecutor>);
}

} // namespace
} // namespace fastecu::flash
