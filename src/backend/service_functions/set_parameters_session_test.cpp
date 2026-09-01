#include "src/backend/service_functions/set_parameters_session.h"

#include <array>

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

// Hand-derived full wire frames for sample(). These literals deliberately do
// not use tcu_parameter_writes(), framed(), or SsmProtocol::addHeader(), so a
// wrong table address or framing helper cannot make both sides agree.
const std::array<bytes::Bytes, 12> kLiteralFrames{
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x01, 0x6c, 0x33, 0xe5},
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x01, 0x6d, 0x22, 0xd5},
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x01, 0x6e, 0x11, 0xc5},
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x01, 0x6f, 0x44, 0xf9},
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x01, 0x70, 0xbe, 0x74},
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x01, 0x71, 0xef, 0xa6},
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x01, 0xbc, 0x55, 0x57},
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x01, 0xbd, 0x66, 0x69},
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x01, 0xbe, 0x77, 0x7b},
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x01, 0xbf, 0x88, 0x8d},
    // legacy :453-479 -- B8 00 00 EC 55/AA, each independently framed.
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x00, 0xec, 0x55, 0x86},
    bytes::Bytes{0x80, 0x18, 0xf0, 0x05, 0xb8, 0x00, 0x00, 0xec, 0xaa, 0xdb},
};

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
    for (const bytes::Bytes& frame : kLiteralFrames)
    {
        transport.expectWrite(frame);
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

TEST(SetParametersSession, StopsAfterOneSilentReadWithoutRetrying)
{
    Fixture fixture;
    fixture.transport.expectWrite(kLiteralFrames[0]);
    fixture.transport.queue_no_frame();

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);

    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Timeout);
    EXPECT_TRUE(fixture.transport.scriptConsumed()); // exactly one live write
}

TEST(SetParametersSession, StopsAfterOneNonPositiveResponseWithoutRetrying)
{
    // legacy :227 returns STATUS_ERROR without commenting the return out.
    Fixture fixture;
    fixture.transport.expectWrite(kLiteralFrames[0]);
    fixture.transport.queueRead(bytes::Bytes{0x80, 0xf0, 0x18, 0x02, 0x7f, 0xb8, 0x11});

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::BadResponse);
    EXPECT_TRUE(fixture.transport.scriptConsumed()); // negative response is terminal
}

TEST(SetParametersSession, StopsAfterOneMalformedResponseWithoutRetrying)
{
    Fixture fixture;
    fixture.transport.expectWrite(kLiteralFrames[0]);
    fixture.transport.queueRead(bytes::Bytes{0x80, 0xf0, 0x18, 0x02});

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::BadResponse);
    EXPECT_TRUE(fixture.transport.scriptConsumed());
}

TEST(SetParametersSession, StopsAfterOneSilentCommitWithoutRetrying)
{
    Fixture fixture;
    for (std::size_t index = 0; index < 10; ++index)
    {
        fixture.transport.expectWrite(kLiteralFrames[index]);
        fixture.transport.queueRead(ack());
    }
    fixture.transport.expectWrite(kLiteralFrames[10]);
    fixture.transport.queue_no_frame();

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Timeout);
    EXPECT_TRUE(fixture.transport.scriptConsumed()); // commit value 0x55 sent once
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
