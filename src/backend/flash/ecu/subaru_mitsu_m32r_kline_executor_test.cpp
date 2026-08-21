#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.h"

#include <gtest/gtest.h>

#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using namespace fastecu;
using namespace fastecu::flash;

bytes::Bytes frame(bytes::Bytes payload)
{
    return SsmProtocol::addHeader(payload, 0xf0, 0x10);
}

void scriptHandshake(ScriptedKlineFlashTransport& transport)
{
    transport.expectWrite(frame({0xbf}));
    transport.queueRead(bytes::Bytes{0x80, 0xf0, 0x10, 0x09, 0xff, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0});
    transport.expectWrite(frame({0x81}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0xc1});
    transport.expectWrite(frame({0x83, 0x00}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0xc3});
    transport.expectWrite(frame({0x27, 0x01}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x67, 0x01, 0x12, 0x34, 0x56, 0x78});
    // Hand-derived seed 0x12345678 -> key 0xF7ABD485.
    transport.expectWrite(frame({0x27, 0x02, 0xf7, 0xab, 0xd4, 0x85}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x67, 0x02});
    transport.expectWrite(frame({0x10, 0x85, 0x02}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x50});
}

bytes::Bytes encryptedImage(bytes::ByteView image)
{
    static constexpr std::uint16_t index[] = {0x25b5, 0x3875, 0xca11, 0x2680};
    static constexpr std::uint8_t transform[] = {0x5, 0x6, 0x7, 0x1, 0x9, 0xc, 0xd, 0x8, 0xa, 0xd, 0x2,
                                                 0xb, 0xf, 0x4, 0x0, 0x3, 0xb, 0x4, 0x6, 0x0, 0xf, 0x2,
                                                 0xd, 0x9, 0x5, 0xc, 0x1, 0xa, 0x3, 0xd, 0xe, 0x8};
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()), index, transform);
}

TEST(SubaruMitsuM32rKlineExecutor, RejectsFamilyMismatchBeforeIo)
{
    auto plan =
        build_mitsu_colt_m32r_can_plan(FlashOperation::Read, "mitsu_ecu_m32r_can", "M32R_384KB_1block", std::nullopt);
    ASSERT_TRUE(plan.has_value());
    SubaruMitsuM32rKlineExecutor executor;
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_FALSE(transport.last_config_.has_value());
    EXPECT_EQ(transport.close_call_count_, 0);
}

TEST(SubaruMitsuM32rKlineExecutor, CancellationBeforeSetupPerformsNoIo)
{
    auto plan = build_subaru_mitsu_m32r_kline_plan(FlashOperation::Read, "sub_ecu_mitsu_m32r_kline",
                                                   "M32R_512KB_4blocks", std::nullopt);
    ASSERT_TRUE(plan.has_value());
    SubaruMitsuM32rKlineExecutor executor;
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    ManualCancellationToken cancellation;
    cancellation.cancel();
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_FALSE(transport.last_config_.has_value());
}

TEST(SubaruMitsuM32rKlineExecutor, MapsMissingMalformedAndTransportFailureResponses)
{
    for (const ErrorKind expected : {ErrorKind::Timeout, ErrorKind::BadResponse, ErrorKind::Disconnected})
    {
        auto plan = build_subaru_mitsu_m32r_kline_plan(FlashOperation::Read, "sub_ecu_mitsu_m32r_kline",
                                                       "M32R_512KB_4blocks", std::nullopt);
        ASSERT_TRUE(plan.has_value());
        ScriptedKlineFlashTransport transport;
        transport.expectWrite(frame({0xbf}));
        if (expected == ErrorKind::Timeout)
        {
            transport.queue_no_frame();
        }
        else if (expected == ErrorKind::BadResponse)
        {
            transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x7f});
        }
        else
        {
            transport.queue_error(ErrorKind::Disconnected, "adapter disconnected");
        }
        SubaruMitsuM32rKlineExecutor executor;
        FakeClock clock;
        ManualCancellationToken cancellation;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, expected);
        EXPECT_EQ(transport.close_call_count_, 1);
    }
}

TEST(SubaruMitsuM32rKlineExecutor, ReadsAllUserspaceChunksAndSynthesizesBootPrefix)
{
    auto plan = build_subaru_mitsu_m32r_kline_plan(FlashOperation::Read, "sub_ecu_mitsu_m32r_kline",
                                                   "M32R_512KB_4blocks", std::nullopt);
    ASSERT_TRUE(plan.has_value());
    SubaruMitsuM32rKlineExecutor executor;
    ScriptedKlineFlashTransport transport;
    scriptHandshake(transport);
    for (std::uint32_t address = 0x8000; address < 0x80000; address += 0x80)
    {
        transport.expectWrite(frame({0xa0, 0, 0x20, static_cast<bytes::Byte>(address >> 16),
                                     static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address), 0x7f}));
        bytes::Bytes response(134, 0x5a);
        response[4] = 0xe0;
        transport.queueRead(response);
    }
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), 0x80000u);
    EXPECT_TRUE(std::all_of(result->read_bytes->begin(), result->read_bytes->begin() + 0x8000,
                            [](bytes::Byte value) { return value == 0xff; }));
    EXPECT_TRUE(std::all_of(result->read_bytes->begin() + 0x8000, result->read_bytes->end(),
                            [](bytes::Byte value) { return value == 0x5a; }));
    EXPECT_EQ(result->rom_id, std::string("123456789A_"));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
    ASSERT_TRUE(transport.last_config_.has_value());
    EXPECT_EQ(transport.last_config_->baud, 4800);
}

TEST(SubaruMitsuM32rKlineExecutor, WritesEveryEncryptedChunkAndToleratesTransferAcks)
{
    bytes::Bytes image(0x80000);
    for (std::size_t i = 0; i < image.size(); ++i)
    {
        image[i] = static_cast<bytes::Byte>(i);
    }
    const bytes::Bytes encrypted = encryptedImage(image);
    auto plan = build_subaru_mitsu_m32r_kline_plan(FlashOperation::Write, "sub_ecu_mitsu_m32r_kline",
                                                   "M32R_512KB_4blocks", image);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    scriptHandshake(transport);
    transport.expectWrite(frame({0x34, 0, 0, 0, 0x04, 0x07, 0x80, 0}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x74});
    transport.expectWrite(frame({0x31, 0x02, 0x0f, 0xff, 0xff, 0xff}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x71});
    for (std::uint32_t address = 0x8000; address < 0x80000; address += 0x80)
    {
        bytes::Bytes request{0x36, static_cast<bytes::Byte>(address >> 16), static_cast<bytes::Byte>(address >> 8),
                             static_cast<bytes::Byte>(address)};
        request.insert(request.end(), encrypted.begin() + address, encrypted.begin() + address + 0x80);
        transport.expectWrite(frame(request));
        if (((address - 0x8000) / 0x80) % 2 == 0)
        {
            transport.queue_no_frame();
        }
        else
        {
            transport.queueRead(bytes::Bytes{0xde, 0xad});
        }
    }
    transport.expectWrite(frame({0x31, 0x01, 0x02}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x71, 0x01, 0x02});
    SubaruMitsuM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, std::vector<int>{15625});
    EXPECT_EQ(transport.close_call_count_, 1);
}
} // namespace
