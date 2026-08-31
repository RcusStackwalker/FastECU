#include "src/backend/service_functions/service_function_types.h"

#include <gtest/gtest.h>

namespace fastecu::service_functions
{
namespace
{

TEST(SsmTransportConfig, DefaultsToTheTcuIso15765Pair)
{
    const SsmTransportConfig config;
    EXPECT_EQ(config.framing, SsmTransportConfig::Framing::Iso15765);
    EXPECT_EQ(config.bitrate_or_baud, 500000);
    EXPECT_EQ(config.request_id, 0x7e1U);
    EXPECT_EQ(config.response_id, 0x7e9U);
    EXPECT_FALSE(config.add_iso14230_header);
}

TEST(SsmTransportConfig, TesterAndTargetAreZeroUnlessKline)
{
    // The two ISO-15765 sessions never reference them: legacy sets them only
    // on the K-Line path (legacy :151-152) and uses them only there (:215).
    const SsmTransportConfig config;
    EXPECT_EQ(config.tester_id, 0x00);
    EXPECT_EQ(config.target_id, 0x00);
}

TEST(ServiceFunctionStep, HoldsGateCompletedAndFailedAlternatives)
{
    const ServiceFunctionStep gate = GateStep{OperatorGateId::RelearnEngineRunning};
    ASSERT_TRUE(std::holds_alternative<GateStep>(gate));
    EXPECT_EQ(std::get<GateStep>(gate).id, OperatorGateId::RelearnEngineRunning);

    const ServiceFunctionStep done = CompletedStep{TcuParameterReadout{}};
    ASSERT_TRUE(std::holds_alternative<CompletedStep>(done));

    const ServiceFunctionStep bad = FailedStep{Error{ErrorKind::BadResponse, "nope"}};
    ASSERT_TRUE(std::holds_alternative<FailedStep>(bad));
    EXPECT_EQ(std::get<FailedStep>(bad).error.kind, ErrorKind::BadResponse);
}

TEST(TcuParameterReadout, ValueTypesEncodeTheLegacyPromptBounds)
{
    // legacy :162-202 -- eight 0-255 prompts and one 0-65535 prompt. The value
    // model enforces those bounds instead of a runtime range check.
    static_assert(sizeof(TcuParameterReadout::input_clutch) == 1);
    static_assert(sizeof(TcuParameterReadout::awd_clutch_torque) == 2);
    SUCCEED();
}

} // namespace
} // namespace fastecu::service_functions
