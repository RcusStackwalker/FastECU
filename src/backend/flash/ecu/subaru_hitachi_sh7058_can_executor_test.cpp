#include "src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor.h"

#include <gtest/gtest.h>

#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace fastecu::flash
{
namespace
{
bytes::Bytes request(bytes::ByteView payload, std::uint32_t id = 0x7e0)
{
    return bytes::composeBe(id, payload);
}
bytes::Bytes response(bytes::ByteView payload)
{
    bytes::Bytes frame{0, 0, 7, 0xe8};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}
void script_inactive_connect(ScriptedCanFlashTransport& transport, bool bench)
{
    transport.exchange(request(bytes::Bytes{0xb7}), response(bytes::Bytes{0x7f, 0xb7, 0x11}));
    transport.exchange(request(bytes::Bytes{0xaa}), response(bytes::Bytes{0xea}));
    transport.exchange(request(bytes::Bytes{0x09, 0x02}), response(bytes::Bytes{0x49, 0x02}));
    transport.exchange(request(bytes::Bytes{0x09, 0x04}), response(bytes::Bytes{0x49, 0x04}));
    transport.exchange(request(bytes::Bytes{0x09, 0x06}), response(bytes::Bytes{0x49, 0x06}));
    transport.exchange(request(bytes::Bytes{0xa8, 0, 0, 0, 0xd7}),
                       response(bench ? bytes::Bytes{0, 0xa0} : bytes::Bytes{0, 0}));
    if (bench)
    {
        transport.exchange(request(bytes::Bytes{0x10, 0x43}), response(bytes::Bytes{0x50, 0x43}));
    }
    else
    {
        struct Step
        {
            std::uint32_t id;
            bytes::Bytes payload;
        };
        const std::array<Step, 9> steps{{
            {0x7e0, {0xa8, 0, 0, 1, 0x3b}},
            {0x7df, {0x10, 0x03}},
            {0x7e1, {0x04}},
            {0x7b0, {0x10, 0x03}},
            {0x7b0, {0x85, 0x02}},
            {0x7df, {0x85, 0x02}},
            {0x7b0, {0x85, 0x02}},
            {0x7df, {0x85, 0x02}},
            {0x7df, {0x28, 0x03, 0x01}},
        }};
        for (const auto& step : steps)
        {
            transport.exchange(request(step.payload, step.id), response(bytes::Bytes{0x50}));
        }
    }
    transport.exchange(request(bytes::Bytes{0x27, 0x01}), response(bytes::Bytes{0x67, 0x01, 0x11, 0x22, 0x33, 0x44}));
    // Independent seed vector from the legacy SH7058 table: 11 22 33 44 -> 61 FB 90 90.
    transport.exchange(request(bytes::Bytes{0x27, 0x02, 0x61, 0xfb, 0x90, 0x90}), response(bytes::Bytes{0x67, 0x02}));
    if (bench)
    {
        transport.exchange(request(bytes::Bytes{0x10, 0x42}), response(bytes::Bytes{0x50, 0x42}));
    }
    else
    {
        for (const bytes::Bytes& payload :
             {bytes::Bytes{0xa8, 0, 0, 0, 0xd5}, bytes::Bytes{0xa8, 0, 0, 1, 0x3b}, bytes::Bytes{0xa8, 0, 0, 0, 0x1c},
              bytes::Bytes{0xa8, 0, 0, 0, 0x0e, 0, 0, 0x0f}})
        {
            transport.exchange(request(payload), response(bytes::Bytes{0x50}));
        }
        transport.exchange(request(bytes::Bytes{0x10, 0x02}), response(bytes::Bytes{0x50, 0x02}));
    }
    transport.exchange(request(bytes::Bytes{0x34, 4, 0x33, 0, 0, 0, 0x10, 0, 0}),
                       response(bytes::Bytes{0x74, 0x20, 0x01, 0x04}));
}
} // namespace
TEST(SubaruHitachiSh7058CanExecutor, MissingProbeStopsBeforeErase)
{
    auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 bytes::Bytes(0x100000));
    ASSERT_TRUE(plan.has_value());
    SubaruHitachiSh7058CanExecutor executor;
    ScriptedCanFlashTransport transport;
    transport.expectWrite(bytes::Bytes{0, 0, 7, 0xe0, 0xb7});
    transport.queue_no_frame();
    FakeClock clock;
    FakeCancellationToken cancel;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancel, events);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruHitachiSh7058CanExecutor, ActiveKernelWritesAll4096Frames)
{
    bytes::Bytes image(0x100000, 0x5a);
    auto plan =
        build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block", image);
    ASSERT_TRUE(plan.has_value());
    constexpr std::array<std::uint16_t, 4> keys{0x14ca, 0x77f4, 0x973c, 0xf50e};
    const auto encrypted = SsmProtocol::calculatePayload(image, 0x100000, keys, SsmProtocol::kIndexTransformationStock);
    ASSERT_GE(encrypted.size(), 4U);
    EXPECT_EQ(bytes::Bytes(encrypted.begin(), encrypted.begin() + 4), (bytes::Bytes{0x08, 0x03, 0xfd, 0x11}));
    const auto frame = [](bytes::ByteView payload)
    {
        bytes::Bytes value{0, 0, 7, 0xe0};
        value.insert(value.end(), payload.begin(), payload.end());
        return value;
    };
    ScriptedCanFlashTransport transport;
    transport.exchange(frame(bytes::Bytes{0xb7}), bytes::Bytes{0, 0, 7, 0xe8, 0x7f, 0xb7, 0x13});
    transport.exchange(frame(bytes::Bytes{0x31, 1, 2, 1, 0x0f, 0xff, 0xff, 0xff}),
                       bytes::Bytes{0, 0, 7, 0xe8, 0x71, 1, 2});
    transport.exchange(frame(bytes::Bytes{0x34, 4, 0x33, 0, 0, 0, 0x10, 0, 0}), bytes::Bytes{0, 0, 7, 0xe8, 0x74});
    for (std::uint32_t address = 0; address < 0x100000; address += 0x100)
    {
        bytes::Bytes request = bytes::composeBe(bytes::Byte{0xb6}, bytes::u24(address),
                                                bytes::ByteView(encrypted).subspan(address, 0x100));
        transport.exchange(frame(request), bytes::Bytes{0, 0, 7, 0xe8, 0xf6});
    }
    transport.exchange(frame(bytes::Bytes{0x37}), bytes::Bytes{0, 0, 7, 0xe8, 0x77});
    transport.exchange(frame(bytes::Bytes{0x31, 1, 2, 2, 1}), bytes::Bytes{0, 0, 7, 0xe8, 0x71, 1, 2});
    SubaruHitachiSh7058CanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancel;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancel, events);
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiSh7058CanExecutor, BenchAndInCarConnectStopOnEraseTransportFailure)
{
    for (const bool bench : {true, false})
    {
        auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can",
                                                     "SH7058_1block", bytes::Bytes(0x100000));
        ASSERT_TRUE(plan.has_value());
        ScriptedCanFlashTransport transport;
        script_inactive_connect(transport, bench);
        transport.expectWrite(request(bytes::Bytes{0x31, 1, 2, 1, 0x0f, 0xff, 0xff, 0xff}));
        transport.queue_error(ErrorKind::Disconnected);
        SubaruHitachiSh7058CanExecutor executor;
        FakeClock clock;
        FakeCancellationToken cancel;
        RecordingEventSink events;
        auto result = executor.execute(*plan, transport, clock, cancel, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruHitachiSh7058CanExecutor, RejectsMalformedSeedBeforeKernelJumpOrErase)
{
    auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 bytes::Bytes(0x100000));
    ASSERT_TRUE(plan.has_value());
    ScriptedCanFlashTransport transport;
    transport.exchange(request(bytes::Bytes{0xb7}), response(bytes::Bytes{0x7f, 0xb7, 0x11}));
    transport.exchange(request(bytes::Bytes{0xaa}), response(bytes::Bytes{0xea}));
    transport.exchange(request(bytes::Bytes{0x09, 0x02}), response(bytes::Bytes{0x49, 0x02}));
    transport.exchange(request(bytes::Bytes{0x09, 0x04}), response(bytes::Bytes{0x49, 0x04}));
    transport.exchange(request(bytes::Bytes{0x09, 0x06}), response(bytes::Bytes{0x49, 0x06}));
    transport.exchange(request(bytes::Bytes{0xa8, 0, 0, 0, 0xd7}), response(bytes::Bytes{0, 0xa0}));
    transport.exchange(request(bytes::Bytes{0x10, 0x43}), response(bytes::Bytes{0x50, 0x43}));
    transport.exchange(request(bytes::Bytes{0x27, 0x01}), response(bytes::Bytes{0x67, 0x01, 0x11}));
    SubaruHitachiSh7058CanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancel;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancel, events);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiSh7058CanExecutor, RetriesTransferSetupSixTimesThenStopsBeforeFrameWrites)
{
    auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 bytes::Bytes(0x100000));
    ASSERT_TRUE(plan.has_value());
    ScriptedCanFlashTransport transport;
    transport.exchange(request(bytes::Bytes{0xb7}), response(bytes::Bytes{0x7f, 0xb7, 0x13}));
    transport.exchange(request(bytes::Bytes{0x31, 1, 2, 1, 0x0f, 0xff, 0xff, 0xff}),
                       response(bytes::Bytes{0x71, 1, 2}));
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        transport.exchange(request(bytes::Bytes{0x34, 4, 0x33, 0, 0, 0, 0x10, 0, 0}),
                           response(bytes::Bytes{0x7f, 0x34, 0x13}));
    }
    SubaruHitachiSh7058CanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancel;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancel, events);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiSh7058CanExecutor, CancellationAfterFirstFramePreventsLaterProgrammingCommands)
{
    auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 bytes::Bytes(0x100000));
    ASSERT_TRUE(plan.has_value());
    ScriptedCanFlashTransport transport;
    transport.exchange(request(bytes::Bytes{0xb7}), response(bytes::Bytes{0x7f, 0xb7, 0x13}));
    transport.exchange(request(bytes::Bytes{0x31, 1, 2, 1, 0x0f, 0xff, 0xff, 0xff}),
                       response(bytes::Bytes{0x71, 1, 2}));
    transport.exchange(request(bytes::Bytes{0x34, 4, 0x33, 0, 0, 0, 0x10, 0, 0}), response(bytes::Bytes{0x74}));
    FakeCancellationToken cancel;
    cancel.set_predicate([&transport] { return transport.writesConsumed() >= 3; });
    SubaruHitachiSh7058CanExecutor executor;
    FakeClock clock;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancel, events);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 3U);
}

TEST(SubaruHitachiSh7058CanExecutor, EraseAcknowledgementCanArriveOnLaterRead)
{
    auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 bytes::Bytes(0x100000));
    ASSERT_TRUE(plan.has_value());
    ScriptedCanFlashTransport transport;
    transport.exchange(request(bytes::Bytes{0xb7}), response(bytes::Bytes{0x7f, 0xb7, 0x13}));
    transport.exchange(request(bytes::Bytes{0x31, 1, 2, 1, 0x0f, 0xff, 0xff, 0xff}),
                       response(bytes::Bytes{0x7f, 0x31, 0x78}));
    transport.queue_no_frame();
    transport.queueRead(response(bytes::Bytes{0x71, 1, 2}));
    transport.expectWrite(request(bytes::Bytes{0x34, 4, 0x33, 0, 0, 0, 0x10, 0, 0}));
    transport.queue_error(ErrorKind::Disconnected);
    SubaruHitachiSh7058CanExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancel;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancel, events);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_TRUE(transport.scriptConsumed());
}
} // namespace fastecu::flash
