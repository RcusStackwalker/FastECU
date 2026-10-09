#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/service_functions/read_parameters_session.h"

#include <gtest/gtest.h>

#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/scripted_ssm_transport.h"

namespace fastecu::service_functions
{
namespace
{

// legacy :534-570 -- 0xA8 read of ten addresses behind the 0x7E1 envelope.
const bytes::Bytes kRequest{
    0x00, 0x00, 0x07, 0xe1, 0xa8, 0x00, 0x00, 0x01, 0x6c, 0x00, 0x01, 0x6d, 0x00, 0x01, 0x6e, 0x00, 0x01, 0x6f,
    0x00, 0x01, 0x70, 0x00, 0x01, 0x71, 0x00, 0x01, 0xbc, 0x00, 0x01, 0xbd, 0x00, 0x01, 0xbe, 0x00, 0x01, 0xbf,
};

// Four envelope bytes, 0xE8, then the ten data bytes at 5..14.
bytes::Bytes GoodReply()
{
    return {0x00, 0x00, 0x07, 0xe9, 0xe8, 0x11, 0x22, 0x33, 0x44, 0xbe, 0xef, 0x55, 0x66, 0x77, 0x88};
}

struct Fixture
{
    ScriptedSsmTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    NullEventSink events;
    ReadParametersSession session{"sub_tcu_denso_sh7058_can"};
};

TEST(ReadParametersSession, RequiresTheIso15765TcuPair)
{
    const Fixture fixture;
    const auto setup = fixture.session.TransportSetup();
    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->framing, SsmTransportConfig::Framing::kIso15765);
    EXPECT_EQ(setup->bitrate_or_baud, 500000);
    EXPECT_EQ(setup->request_id, 0x7e1U);
    EXPECT_EQ(setup->response_id, 0x7e9U);
}

TEST(ReadParametersSession, RejectsAnUnknownProtocolBeforeAnyIo)
{
    const ReadParametersSession session{"sub_ecu_denso_sh7058_can"};
    ASSERT_THAT(session.TransportSetup(), fastecu::testing::IsErr(ErrorKind::kUnsupported));
}

TEST(ReadParametersSession, DecodesTheNineValuesFromBytesFiveToFourteen)
{
    Fixture fixture;
    fixture.transport.ExpectWrite(kRequest);
    fixture.transport.QueueRead(GoodReply());

    const auto step = fixture.session.Resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);

    ASSERT_TRUE(std::holds_alternative<CompletedStep>(step));
    const auto& readout = std::get<TcuParameterReadout>(std::get<CompletedStep>(step).outcome);
    EXPECT_EQ(readout.input_clutch, 0x11);
    EXPECT_EQ(readout.high_low_reverse_clutch, 0x22);
    EXPECT_EQ(readout.direct_clutch, 0x33);
    EXPECT_EQ(readout.front_brake, 0x44);
    EXPECT_EQ(readout.awd_clutch_torque, 0xbeefU);
    EXPECT_EQ(readout.forward_brake, 0x55);
    EXPECT_EQ(readout.four_wheel_drive, 0x66);
    EXPECT_EQ(readout.line_pressure, 0x77);
    EXPECT_EQ(readout.temperature_basis, 0x88);
    EXPECT_TRUE(fixture.transport.Ok());
    EXPECT_TRUE(fixture.transport.ScriptConsumed());
}

TEST(ReadParametersSession, AcceptsE8WhichTheLegacyRetryLoopNeverCould)
{
    // legacy :571-608 sets responseOK only on 0xF8 and then demands 0xE8, so
    // the legacy always returns STATUS_ERROR. This is the corrected path.
    Fixture fixture;
    fixture.transport.ExpectWrite(kRequest);
    fixture.transport.QueueRead(GoodReply());

    const auto step = fixture.session.Resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    EXPECT_TRUE(std::holds_alternative<CompletedStep>(step));
}

TEST(ReadParametersSession, RetriesUpToSixTimes)
{
    // legacy :571 -- while (try_count < 6 && !responseOK).
    Fixture fixture;
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        fixture.transport.ExpectWrite(kRequest);
    }
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        fixture.transport.QueueNoFrame();
    }
    fixture.transport.QueueRead(GoodReply());

    const auto step = fixture.session.Resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    EXPECT_TRUE(std::holds_alternative<CompletedStep>(step));
    EXPECT_TRUE(fixture.transport.ScriptConsumed());
}

TEST(ReadParametersSession, TimesOutWhenAllSixAttemptsAreSilent)
{
    Fixture fixture;
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        fixture.transport.ExpectWrite(kRequest);
        fixture.transport.QueueNoFrame();
    }

    const auto step = fixture.session.Resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::kTimeout);
}

TEST(ReadParametersSession, RejectsAFrameShorterThanFifteenBytes)
{
    // legacy :592 guards on length() > 10 but :624 indexes byte 14.
    Fixture fixture;
    fixture.transport.ExpectWrite(kRequest);
    fixture.transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xe9, 0xe8, 0x11, 0x22, 0x33, 0x44, 0xbe, 0xef, 0x55});

    const auto step = fixture.session.Resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::kBadResponse);
}

TEST(ReadParametersSession, ReportsANegativeResponseAsBadResponse)
{
    Fixture fixture;
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        fixture.transport.ExpectWrite(kRequest);
        fixture.transport.QueueRead(
            bytes::Bytes{0x00, 0x00, 0x07, 0xe9, 0x7f, 0xa8, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    }

    const auto step = fixture.session.Resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::kBadResponse);
}

TEST(ReadParametersSession, ReportsADroppedTransportAsDisconnected)
{
    Fixture fixture;
    fixture.transport.ExpectWrite(kRequest);
    fixture.transport.QueueError(ErrorKind::kDisconnected, "adapter gone");

    const auto step = fixture.session.Resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::kDisconnected);
}

TEST(ReadParametersSession, ObservesCancellation)
{
    Fixture fixture;
    fixture.cancellation.SetCancelled(true);

    const auto step = fixture.session.Resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::kCancelled);
}

TEST(ReadParametersSession, SubmitIsInternalBecauseItHasNoGates)
{
    Fixture fixture;
    fixture.session.Submit(GateResponse::kAccept);
    fixture.transport.ExpectWrite(kRequest);
    fixture.transport.QueueRead(GoodReply());

    const auto step = fixture.session.Resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::kInternal);
}

} // namespace
} // namespace fastecu::service_functions
