#include "src/backend/service_functions/set_parameters_session.h"

#include <gtest/gtest.h>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/scripted_ssm_transport.h"

namespace fastecu::service_functions
{
namespace
{

TcuParameterValues sample()
{
    return TcuParameterValues{
        .correction_1to2 = 0x11,
        .correction_2to3 = 0x22,
        .correction_3to4 = 0x33,
        .correction_4to5 = 0x44,
        .correction_forward_brake = 0x55,
        .correction_four_wheel_drive = 0x66,
        .correction_line_pressure = 0x77,
        .temperature_basis = 0x88,
        .torque_correction_awd = 0xbeef,
    };
}

// legacy :210-215 -- payload 0xB8 + 24-bit address + value, framed once.
bytes::Bytes framed(std::uint32_t address, bytes::Byte value)
{
    bytes::Bytes payload{0xb8};
    bytes::appendU24Be(payload, address);
    payload.push_back(value);
    return SsmProtocol::addHeader(payload, 0xf0, 0x18);
}

bytes::Bytes ack()
{
    // Positive response 0xF8 at index 4, behind the SSM header.
    return {0x80, 0xf0, 0x18, 0x02, 0xf8, 0x00, 0x00};
}

struct Fixture
{
    ScriptedSsmTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    NullEventSink events;
    SetParametersSession session{"sub_tcu_denso_sh7058_can", sample()};
};

void scriptAllTwelve(ScriptedSsmTransport& transport)
{
    for (const auto& write : tcu_parameter_writes(sample()))
    {
        transport.expectWrite(framed(write.address, write.value));
        transport.queueRead(ack());
    }
}

TEST(SetParametersSession, RequiresTheKlineConfigurationNotTheCanOne)
{
    // legacy :141-152 -- K-Line, 4800 baud, tester 0xF0, target 0x18, and the
    // driver's ISO14230 auto-header off because the session frames its own.
    const Fixture fixture;
    const auto setup = fixture.session.transport_setup();
    ASSERT_TRUE(setup.has_value());
    EXPECT_EQ(setup->framing, SsmTransportConfig::Framing::Kline14230);
    EXPECT_EQ(setup->bitrate_or_baud, 4800);
    EXPECT_EQ(setup->tester_id, 0xf0);
    EXPECT_EQ(setup->target_id, 0x18);
    EXPECT_FALSE(setup->add_iso14230_header);
}

TEST(SetParametersSession, RejectsAnUnknownProtocolBeforeAnyIo)
{
    const SetParametersSession session{"sub_ecu_denso_sh7058_can", sample()};
    const auto setup = session.transport_setup();
    ASSERT_FALSE(setup.has_value());
    EXPECT_EQ(setup.error().kind, ErrorKind::Unsupported);
}

TEST(SetParametersSession, WritesAllTwelveFramesEachFramedExactlyOnce)
{
    // The legacy corrupts every frame after the first (:215 reassigns `output`
    // to the framed array; :237 onward mutate the length byte and service ID
    // and re-frame it), so it aborts on write 2 leaving the TCU half set.
    Fixture fixture;
    scriptAllTwelve(fixture.transport);

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);

    ASSERT_TRUE(std::holds_alternative<CompletedStep>(step));
    EXPECT_EQ(std::get<SetParametersOutcome>(std::get<CompletedStep>(step).outcome).frames_written, 12);
    EXPECT_TRUE(fixture.transport.ok());
    EXPECT_TRUE(fixture.transport.scriptConsumed());
}

TEST(SetParametersSession, FirstFrameMatchesTheOneFrameTheLegacyGetsRight)
{
    // legacy :210-215 is the only well-formed legacy frame; ours must equal it
    // byte for byte: 80 18 F0 05 B8 00 01 6C 33 <sum8>.
    const bytes::Bytes first = framed(0x00016c, 0x33);
    ASSERT_EQ(first.size(), 10U);
    EXPECT_EQ(first[0], 0x80);
    EXPECT_EQ(first[1], 0x18);
    EXPECT_EQ(first[2], 0xf0);
    EXPECT_EQ(first[3], 0x05);
    EXPECT_EQ(first[4], 0xb8);
    EXPECT_EQ(first[8], 0x33);

    Fixture fixture;
    scriptAllTwelve(fixture.transport);
    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    EXPECT_TRUE(std::holds_alternative<CompletedStep>(step));
}

TEST(SetParametersSession, StopsAtTheFirstNonPositiveResponse)
{
    // legacy :227 returns STATUS_ERROR without commenting the return out.
    Fixture fixture;
    const auto writes = tcu_parameter_writes(sample());
    fixture.transport.expectWrite(framed(writes[0].address, writes[0].value));
    fixture.transport.queueRead(ack());
    fixture.transport.expectWrite(framed(writes[1].address, writes[1].value));
    fixture.transport.queueRead(bytes::Bytes{0x80, 0xf0, 0x18, 0x02, 0x7f, 0xb8, 0x11});

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::BadResponse);
}

TEST(SetParametersSession, TreatsASilentReadAsTimeout)
{
    Fixture fixture;
    const auto writes = tcu_parameter_writes(sample());
    fixture.transport.expectWrite(framed(writes[0].address, writes[0].value));
    fixture.transport.queue_no_frame();

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Timeout);
}

TEST(SetParametersSession, ReportsADroppedTransportAsDisconnected)
{
    Fixture fixture;
    const auto writes = tcu_parameter_writes(sample());
    fixture.transport.expectWrite(framed(writes[0].address, writes[0].value));
    fixture.transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Disconnected);
}

TEST(SetParametersSession, ObservesCancellationBetweenWrites)
{
    // resume() polls the token twice per write: once at the top of the loop
    // and once inside ScriptedSsmTransport::read. Tripping on the third check
    // therefore lands after write 1 completes and before write 2 is sent.
    Fixture fixture;
    const auto writes = tcu_parameter_writes(sample());
    fixture.transport.expectWrite(framed(writes[0].address, writes[0].value));
    fixture.transport.queueRead(ack());
    fixture.cancellation.cancel_on_check(3);

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Cancelled);
    EXPECT_TRUE(fixture.transport.scriptConsumed()); // exactly one write went out
}

TEST(SetParametersSession, SubmitIsInternalBecauseItHasNoGates)
{
    Fixture fixture;
    fixture.session.submit(GateResponse::Accept);

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Internal);
}

} // namespace
} // namespace fastecu::service_functions
