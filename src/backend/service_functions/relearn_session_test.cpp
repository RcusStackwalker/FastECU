#include "src/backend/service_functions/relearn_session.h"

#include <gtest/gtest.h>

#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/scripted_ssm_transport.h"

namespace fastecu::service_functions
{
namespace
{

// legacy :655-663 -- 0x7E1 envelope, SID 0xB8, address 0x1FC, value 0x01.
const bytes::Bytes kStepOne{0x00, 0x00, 0x07, 0xe1, 0xb8, 0x00, 0x01, 0xfc, 0x01};
// legacy :699-700 -- same frame with address 0x1FD and value 0x09.
const bytes::Bytes kStepTwo{0x00, 0x00, 0x07, 0xe1, 0xb8, 0x00, 0x01, 0xfd, 0x09};
// legacy :740-747 intends this 12-byte 0xA8 read of 0x1FC and 0x1FD, but
// writes indices 9-11 past the end of the 9-byte step-two buffer.
const bytes::Bytes kPoll{0x00, 0x00, 0x07, 0xe1, 0xa8, 0x00, 0x00, 0x01, 0xfc, 0x00, 0x01, 0xfd};

bytes::Bytes writeAck()
{
    return {0x00, 0x00, 0x07, 0xe9, 0xf8, 0x00};
}

bytes::Bytes pollReply(bytes::Byte first, bytes::Byte second)
{
    return {0x00, 0x00, 0x07, 0xe9, 0xe8, first, second};
}

struct Fixture
{
    ScriptedSsmTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    NullEventSink events;
    RelearnSession session{"sub_tcu_denso_sh7058_can"};

    ServiceFunctionStep step()
    {
        return session.resume(transport, clock, cancellation, events);
    }
};

TEST(RelearnSession, RequiresTheIso15765TcuPair)
{
    const Fixture fixture;
    const auto setup = fixture.session.transport_setup();
    ASSERT_TRUE(setup.has_value());
    EXPECT_EQ(setup->framing, SsmTransportConfig::Framing::Iso15765);
    EXPECT_EQ(setup->request_id, 0x7e1U);
}

TEST(RelearnSession, RejectsAnUnknownProtocolBeforeAnyIo)
{
    const RelearnSession session{"sub_ecu_denso_sh7058_can"};
    const auto setup = session.transport_setup();
    ASSERT_FALSE(setup.has_value());
    EXPECT_EQ(setup.error().kind, ErrorKind::Unsupported);
}

TEST(RelearnSession, AsksForTheStaticSetupGateBeforeAnyIo)
{
    Fixture fixture;
    const auto step = fixture.step();

    ASSERT_TRUE(std::holds_alternative<GateStep>(step));
    EXPECT_EQ(std::get<GateStep>(step).id, OperatorGateId::RelearnStaticSetup);
    EXPECT_TRUE(fixture.transport.scriptConsumed()); // nothing written yet
}

TEST(RelearnSession, AsksForTheEngineRunningGateOnlyAfterStepTwoIsAccepted)
{
    // This ordering is the whole reason the package exists: legacy :735 issues
    // the instruction after the TCU accepts step two, so it cannot be
    // pre-collected the way ConfirmationSpec requires.
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);

    fixture.transport.expectWrite(kStepOne);
    fixture.transport.queueRead(writeAck());
    fixture.transport.expectWrite(kStepTwo);
    fixture.transport.queueRead(writeAck());

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<GateStep>(step));
    EXPECT_EQ(std::get<GateStep>(step).id, OperatorGateId::RelearnEngineRunning);
    EXPECT_TRUE(fixture.transport.ok());
    EXPECT_TRUE(fixture.transport.scriptConsumed());
}

TEST(RelearnSession, PollsWithTheTwelveByteFrameTheLegacyCannotBuild)
{
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);

    fixture.transport.expectWrite(kStepOne);
    fixture.transport.queueRead(writeAck());
    fixture.transport.expectWrite(kStepTwo);
    fixture.transport.queueRead(writeAck());
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);

    for (int poll = 0; poll < 200; ++poll)
    {
        fixture.transport.expectWrite(kPoll);
        fixture.transport.queueRead(pollReply(0x01, 0x02));
    }

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<CompletedStep>(step));
    const auto& outcome = std::get<RelearnOutcome>(std::get<CompletedStep>(step).outcome);
    EXPECT_EQ(outcome.polls_performed, 200);
    EXPECT_EQ(outcome.last_status_frame, pollReply(0x01, 0x02));
    EXPECT_TRUE(fixture.transport.ok());
}

TEST(RelearnSession, ReportsSuccessWhereTheLegacyAlwaysReportedFailure)
{
    // legacy :786 -- the function's last statement is return STATUS_ERROR.
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);
    fixture.transport.expectWrite(kStepOne);
    fixture.transport.queueRead(writeAck());
    fixture.transport.expectWrite(kStepTwo);
    fixture.transport.queueRead(writeAck());
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);
    for (int poll = 0; poll < 200; ++poll)
    {
        fixture.transport.expectWrite(kPoll);
        fixture.transport.queueRead(pollReply(0x00, 0x00));
    }

    EXPECT_TRUE(std::holds_alternative<CompletedStep>(fixture.step()));
}

TEST(RelearnSession, ToleratesABadStepOneResponseAndContinues)
{
    // legacy :688 and :694 -- both returns are commented out, so the legacy
    // logs and proceeds. Tolerance is real behavior and is preserved.
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);

    for (int attempt = 0; attempt < 6; ++attempt)
    {
        fixture.transport.expectWrite(kStepOne);
        fixture.transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xe9, 0x7f, 0xb8, 0x11});
    }
    fixture.transport.expectWrite(kStepTwo);
    fixture.transport.queueRead(writeAck());

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<GateStep>(step));
    EXPECT_EQ(std::get<GateStep>(step).id, OperatorGateId::RelearnEngineRunning);
}

TEST(RelearnSession, RetriesEachWriteStepUpToSixTimes)
{
    // legacy :702 -- while (try_count < 6 && !responseOK).
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);

    for (int attempt = 0; attempt < 5; ++attempt)
    {
        fixture.transport.expectWrite(kStepOne);
        fixture.transport.queue_no_frame();
    }
    fixture.transport.expectWrite(kStepOne);
    fixture.transport.queueRead(writeAck());
    fixture.transport.expectWrite(kStepTwo);
    fixture.transport.queueRead(writeAck());

    EXPECT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    EXPECT_TRUE(fixture.transport.scriptConsumed());
}

TEST(RelearnSession, ADeclinedGateEndsTheSessionAsCancelledNotFailed)
{
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Decline);

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Cancelled);
    EXPECT_TRUE(fixture.transport.scriptConsumed()); // declined before any I/O
}

TEST(RelearnSession, ResumeWithAGateOutstandingIsInternal)
{
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));

    const auto step = fixture.step(); // no submit() in between
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Internal);
}

TEST(RelearnSession, ReportsADroppedTransportAsDisconnected)
{
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);
    fixture.transport.expectWrite(kStepOne);
    fixture.transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Disconnected);
}

TEST(RelearnSession, ObservesCancellationAtTheFirstGate)
{
    Fixture fixture;
    fixture.cancellation.set_cancelled(true);

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Cancelled);
}

} // namespace
} // namespace fastecu::service_functions
