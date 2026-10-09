#include "src/backend/protocol/testing/scripted_ssm_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"
#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(ScriptedSsmTransport, ExpectedTimeoutConsumesQueuedRead)
{
    ScriptedSsmTransport transport;
    fastecu::FakeCancellationToken cancellation;
    transport.ExpectReadTimeout(2ms);
    transport.QueueNoFrame();
    EXPECT_FALSE(transport.ScriptConsumed());
    ASSERT_THAT(transport.Read(2ms, cancellation), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_TRUE(transport.Ok());
}

TEST(ScriptedSsmTransport, WrongTimeoutRejectsReadWithoutConsumingOutcome)
{
    ScriptedSsmTransport transport;
    fastecu::FakeCancellationToken cancellation;
    transport.ExpectReadTimeout(2ms);
    transport.QueueNoFrame();
    EXPECT_THAT(transport.Read(10ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, "unexpected scripted SSM read timeout"));
    EXPECT_FALSE(transport.Ok());
    EXPECT_FALSE(transport.ScriptConsumed());
}
