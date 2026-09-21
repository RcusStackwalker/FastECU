#include "src/backend/flash/ecu/subaru_hitachi_sh7058_kline_executor.h"

#include <gtest/gtest.h>

#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace fastecu::flash
{
TEST(SubaruHitachiSh7058KlineExecutor, RejectsMissingInitializationBeforeReadPages)
{
    auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 std::nullopt);
    ASSERT_TRUE(plan.has_value());
    SubaruHitachiSh7058KlineExecutor executor;
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40});
    transport.queue_no_frame();
    FakeClock clock;
    FakeCancellationToken cancel;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancel, events);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruHitachiSh7058KlineExecutor, ReadsEveryPhysicalPageBeforeReturningRom)
{
    for (const bool already_active : {true, false})
    {
        auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can",
                                                     "SH7058_1block", std::nullopt);
        ASSERT_TRUE(plan.has_value());
        ScriptedKlineFlashTransport transport;
        const auto frame = [](bytes::ByteView payload, bool response)
        {
            return response ? SsmProtocol::addHeader(payload, 0x10, 0xf0) : SsmProtocol::addHeader(payload, 0xf0, 0x10);
        };
        if (already_active)
        {
            transport.exchange(frame(bytes::Bytes{0xbf}, false), frame(bytes::Bytes{0xff}, true));
        }
        else
        {
            transport.expectWrite(frame(bytes::Bytes{0xbf}, false));
            transport.queue_no_frame();
            transport.exchange(frame(bytes::Bytes{0xbf}, false),
                               frame(bytes::Bytes{0xff, 0, 0, 0, 0, 1, 2, 3, 4}, true));
            transport.exchange(frame(bytes::Bytes{0xb8, 0, 0, 0, 0x75}, false), frame(bytes::Bytes{0xf8}, true));
            transport.exchange(frame(bytes::Bytes{0xbf}, false), frame(bytes::Bytes{0xff}, true));
        }
        for (std::uint32_t offset = 0; offset < 0x100000; offset += 0x80)
        {
            const std::uint32_t address = 0x100000 + offset;
            bytes::Bytes request{0xa0,
                                 0,
                                 static_cast<bytes::Byte>(address >> 16),
                                 static_cast<bytes::Byte>(address >> 8),
                                 static_cast<bytes::Byte>(address),
                                 0x7f};
            bytes::Bytes response(129, static_cast<bytes::Byte>(offset / 0x80));
            response[0] = 0xe0;
            transport.exchange(frame(request, false), frame(response, true));
        }
        SubaruHitachiSh7058KlineExecutor executor;
        FakeClock clock;
        FakeCancellationToken cancel;
        RecordingEventSink events;
        auto result = executor.execute(*plan, transport, clock, cancel, events);
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_EQ(result->read_bytes->size(), 0x100000U);
        if (!already_active)
            EXPECT_EQ(result->rom_id, std::optional<std::string>("0001020304_"));
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruHitachiSh7058KlineExecutor, RejectsShortWrongServiceAndBadChecksumPages)
{
    for (int fault = 0; fault < 3; ++fault)
    {
        auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can",
                                                     "SH7058_1block", std::nullopt);
        ASSERT_TRUE(plan.has_value());
        ScriptedKlineFlashTransport transport;
        transport.exchange(SsmProtocol::addHeader(bytes::Bytes{0xbf}, 0xf0, 0x10),
                           SsmProtocol::addHeader(bytes::Bytes{0xff}, 0x10, 0xf0));
        bytes::Bytes payload(fault == 0 ? 128 : 129, 0);
        payload[0] = fault == 1 ? 0xe1 : 0xe0;
        auto response = SsmProtocol::addHeader(payload, 0x10, 0xf0);
        if (fault == 2)
            response.back() ^= 1;
        transport.exchange(SsmProtocol::addHeader(bytes::Bytes{0xa0, 0, 0x10, 0, 0, 0x7f}, 0xf0, 0x10), response);
        SubaruHitachiSh7058KlineExecutor executor;
        FakeClock clock;
        FakeCancellationToken cancel;
        RecordingEventSink events;
        auto result = executor.execute(*plan, transport, clock, cancel, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruHitachiSh7058KlineExecutor, CancellationAndBaudFailureStopBeforeAnyRequest)
{
    auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 std::nullopt);
    ASSERT_TRUE(plan.has_value());
    SubaruHitachiSh7058KlineExecutor executor;
    for (const bool cancel_first : {true, false})
    {
        ScriptedKlineFlashTransport transport;
        if (!cancel_first)
            transport.set_baud_result_ = fail(ErrorKind::Disconnected, "baud failed");
        FakeClock clock;
        FakeCancellationToken cancel(cancel_first);
        RecordingEventSink events;
        auto result = executor.execute(*plan, transport, clock, cancel, events);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, cancel_first ? ErrorKind::Cancelled : ErrorKind::Disconnected);
        EXPECT_EQ(transport.writesConsumed(), 0U);
    }
}

TEST(SubaruHitachiSh7058KlineExecutor, RejectsMalformedIdentityBeforeBaudSwitch)
{
    auto plan = build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 std::nullopt);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    const auto request = SsmProtocol::addHeader(bytes::Bytes{0xbf}, 0xf0, 0x10);
    transport.expectWrite(request);
    transport.queue_no_frame();
    transport.exchange(request, SsmProtocol::addHeader(bytes::Bytes{0xfe}, 0x10, 0xf0));
    SubaruHitachiSh7058KlineExecutor executor;
    FakeClock clock;
    FakeCancellationToken cancel;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancel, events);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}
} // namespace fastecu::flash
