#include "src/backend/flash/ecu/testing/n83m_executor_scenarios.h"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace fastecu::flash::testing
{
namespace
{
using Attempt = ScriptedCanFlashTransport::OperationAttempt;
using Kind = ScriptedCanFlashTransport::OperationKind;

TEST(N83mExecutorScenarios, WrongInterleavingFails)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken token;
    transport.Exchange(bytes::Bytes{1}, bytes::Bytes{2});
    transport.Exchange(bytes::Bytes{3}, bytes::Bytes{4});
    ASSERT_THAT(transport.Write(bytes::Bytes{1}, token), fastecu::testing::IsOk());
    ASSERT_THAT(transport.Write(bytes::Bytes{3}, token), fastecu::testing::IsOk());
    ASSERT_THAT(transport.Read(std::chrono::milliseconds{1}, token), fastecu::testing::IsOk());
    ASSERT_THAT(transport.Read(std::chrono::milliseconds{1}, token), fastecu::testing::IsOk());
    const std::vector<Attempt> expected{
        {Kind::kWrite, true, {}}, {Kind::kRead, true, {}}, {Kind::kWrite, true, {}}, {Kind::kRead, true, {}}};
    EXPECT_NONFATAL_FAILURE(ExpectN83mOperations(transport, expected), "OperationAttempts");
}

TEST(N83mExecutorScenarios, RecoveredViolationFails)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken token;
    transport.ExpectWrite(bytes::Bytes{1});
    ASSERT_THAT(transport.Write(bytes::Bytes{2}, token), fastecu::testing::IsErr(ErrorKind::kInternal));
    ASSERT_THAT(transport.Write(bytes::Bytes{1}, token), fastecu::testing::IsOk());
    const std::vector<Attempt> expected{{Kind::kWrite, false, ErrorKind::kInternal}, {Kind::kWrite, true, {}}};
    EXPECT_NONFATAL_FAILURE(ExpectN83mOperations(transport, expected), "FirstUnexpectedOperation");
}

TEST(N83mExecutorScenarios, FreshRunsDoNotShareState)
{
    for (const bool error : {false, true, false})
    {
        SCOPED_TRACE(error);
        ScriptedCanFlashTransport transport;
        FakeCancellationToken token;
        if (error)
            transport.QueueError(ErrorKind::kInternal, "intentional");
        else
            transport.QueueNoFrame();
        const auto result = transport.Read(std::chrono::milliseconds{1}, token);
        if (error)
            EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kInternal));
        else
            EXPECT_THAT(result, fastecu::testing::IsOk());
        ExpectN83mOperations(transport,
                             {{Kind::kRead, true, error ? std::optional{ErrorKind::kInternal} : std::nullopt}});
    }
}
} // namespace
} // namespace fastecu::flash::testing
