#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.h"

#include <gtest/gtest.h>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/flash_cancellation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using namespace fastecu;
using namespace fastecu::flash;

class TripOnReadTransport final : public ScriptedKlineFlashTransport
{
  public:
    explicit TripOnReadTransport(CancellationSource& source) : source_(source)
    {
    }
    Result<OptionalBytes> read(int timeout, const ICancellationToken& cancellation) override
    {
        auto result = ScriptedKlineFlashTransport::read(timeout, cancellation);
        if (++reads_ == 3)
        {
            source_.trip();
        }
        return result;
    }

  private:
    CancellationSource& source_;
    int reads_ = 0;
};

bytes::Bytes frame(bytes::Bytes payload)
{
    return SsmProtocol::addHeader(payload, 0xf0, 0x10);
}

bytes::Bytes idResponse()
{
    return {0x80, 0xf0, 0x10, 0x09, 0xff, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0};
}

void scriptReadChunks(ScriptedKlineFlashTransport& transport)
{
    for (std::uint32_t logical = 0; logical < 0x80000; logical += 0x80)
    {
        const std::uint32_t address = logical + 0x100000;
        transport.expectWrite(frame({0xa0, 0, 0, static_cast<bytes::Byte>(address >> 16),
                                     static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address), 0x7f}));
        bytes::Bytes response(134, 0x5a);
        response[4] = 0xe0;
        transport.queueRead(response);
    }
}

void scriptAuthenticatedTail(ScriptedKlineFlashTransport& transport)
{
    transport.expectWrite(frame({0x83, 0x00}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0xc3});
    transport.expectWrite(frame({0x27, 0x01}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x67, 0x01, 0x12, 0x34, 0x56, 0x78});
    transport.expectWrite(frame({0x27, 0x02, 0x6c, 0xf8, 0x3a, 0x6a}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x67});
    transport.expectWrite(frame({0x10, 0x85, 0x02}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x50});
}

void scriptNormalAuthenticatedFallback(ScriptedKlineFlashTransport& transport)
{
    transport.expectWrite(frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}));
    transport.queue_no_frame();
    transport.expectWrite(frame({0xbf}));
    transport.queueRead(idResponse());
    transport.expectWrite(frame({0x81}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0xc1});
    transport.expectWrite(frame({0x83, 0x00}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0xc3});
    transport.expectWrite(frame({0x27, 0x01}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x67, 0x01, 0x12, 0x34, 0x56, 0x78});
    transport.expectWrite(frame({0x27, 0x02, 0x6c, 0xf8, 0x3a, 0x6a}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x67, 0x03});
}

bytes::Bytes encryptedImage(bytes::ByteView image)
{
    static constexpr std::uint16_t index[] = {0x78f1, 0x2962, 0x9312, 0x7c03};
    static constexpr std::uint8_t transform[] = {0x5, 0x6, 0x7, 0x1, 0x9, 0xc, 0xd, 0x8, 0xa, 0xd, 0x2,
                                                 0xb, 0xf, 0x4, 0x0, 0x3, 0xb, 0x4, 0x6, 0x0, 0xf, 0x2,
                                                 0xd, 0x9, 0x5, 0xc, 0x1, 0xa, 0x3, 0xd, 0xe, 0x8};
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()), index, transform);
}

void scriptWriteBody(ScriptedKlineFlashTransport& transport, bytes::ByteView image)
{
    transport.expectWrite(frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x74});
    transport.expectWrite(frame({0x31, 0x02, 0x0f, 0xff, 0xff, 0xff}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x71, 0x02});
    const bytes::Bytes encrypted = encryptedImage(image);
    for (std::uint32_t address = 0; address < 0x80000; address += 0x80)
    {
        bytes::Bytes request{0x36, static_cast<bytes::Byte>(address >> 16), static_cast<bytes::Byte>(address >> 8),
                             static_cast<bytes::Byte>(address)};
        request.insert(request.end(), encrypted.begin() + address, encrypted.begin() + address + 0x80);
        transport.expectWrite(frame(request));
        if ((address / 0x80) % 2 == 0)
        {
            transport.queue_no_frame();
        }
        else
        {
            transport.queueRead(bytes::Bytes{0xde, 0xad});
        }
    }
    transport.expectWrite(frame({0x31, 0x01, 0x02}));
    transport.queueRead(bytes::Bytes{0x01});
}

TEST(SubaruHitachiM32rKlineExecutor, ReadsAt38400ProbeAndReturnsLogicalFullRom)
{
    auto plan = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Read, "sub_ecu_hitachi_m32r_kline_recovery",
                                                     "M32R_512KB_1block", std::nullopt);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(frame({0xbf}));
    transport.queueRead(idResponse());
    scriptReadChunks(transport);
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    CancellationSource cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), 0x80000u);
    EXPECT_EQ(result->rom_id, std::string("123456789A_"));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, std::vector<int>{38400});
}

TEST(SubaruHitachiM32rKlineExecutor, RecoveryWakeIsBoundedToOneThousandAttempts)
{
    auto plan = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Write, "sub_ecu_hitachi_m32r_kline_recovery",
                                                     "M32R_512KB_1block", bytes::Bytes(0x80000));
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    for (int i = 0; i < 1000; ++i)
    {
        transport.expectWrite(frame({0x81}));
        transport.queue_no_frame();
    }
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    CancellationSource cancellation;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_EQ(transport.writesConsumed(), 1000u);
}

TEST(SubaruHitachiM32rKlineExecutor, ReadFallsBackThrough4800Initialization)
{
    auto plan = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Read, "sub_ecu_hitachi_m32r_kline",
                                                     "M32R_512KB_1block", std::nullopt);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(frame({0xbf}));
    transport.queue_no_frame();
    transport.expectWrite(frame({0xbf}));
    transport.queueRead(idResponse());
    transport.expectWrite(frame({0xb8, 0, 0, 0, 0x75}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0xf8});
    transport.expectWrite(frame({0xbf}));
    transport.queueRead(idResponse());
    scriptReadChunks(transport);
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    CancellationSource cancellation;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancellation.token(), events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{38400, 4800, 38400}));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiM32rKlineExecutor, NormalWriteUsesActiveObkAndToleratesLegacyAcks)
{
    bytes::Bytes image(0x80000);
    for (std::size_t i = 0; i < image.size(); ++i)
    {
        image[i] = static_cast<bytes::Byte>(i);
    }
    auto plan = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Write, "sub_ecu_hitachi_m32r_kline",
                                                     "M32R_512KB_1block", image);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x74, 0x84});
    scriptWriteBody(transport, image);
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    CancellationSource cancellation;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancellation.token(), events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{15625, 15625}));
}

TEST(SubaruHitachiM32rKlineExecutor, NormalFallbackRequiresSecuritySubfunctionTwo)
{
    const bytes::Bytes image(0x80000);
    auto plan = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Write, "sub_ecu_hitachi_m32r_kline",
                                                     "M32R_512KB_1block", image);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    scriptNormalAuthenticatedFallback(transport);
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    CancellationSource cancellation;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancellation.token(), events);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiM32rKlineExecutor, EraseAcknowledgementAccumulatesBoundedFragments)
{
    const bytes::Bytes image(0x80000, 0xa5);
    auto plan = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Write, "sub_ecu_hitachi_m32r_kline",
                                                     "M32R_512KB_1block", image);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x74, 0x84});
    transport.expectWrite(frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x74});
    transport.expectWrite(frame({0x31, 0x02, 0x0f, 0xff, 0xff, 0xff}));
    transport.queueRead(bytes::Bytes{0, 0, 0});
    transport.queueRead(bytes::Bytes{0, 0x71, 0x02});
    const bytes::Bytes encrypted = encryptedImage(image);
    for (std::uint32_t address = 0; address < 0x80000; address += 0x80)
    {
        bytes::Bytes request{0x36, static_cast<bytes::Byte>(address >> 16), static_cast<bytes::Byte>(address >> 8),
                             static_cast<bytes::Byte>(address)};
        request.insert(request.end(), encrypted.begin() + address, encrypted.begin() + address + 0x80);
        transport.expectWrite(frame(request));
        transport.queue_no_frame();
    }
    transport.expectWrite(frame({0x31, 0x01, 0x02}));
    transport.queueRead(bytes::Bytes{1});
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    CancellationSource cancellation;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancellation.token(), events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruHitachiM32rKlineExecutor, RecoveryWriteWakesAndUsesAuthenticatedSession)
{
    const bytes::Bytes image(0x80000, 0xa5);
    auto plan = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Write, "sub_ecu_hitachi_m32r_kline_recovery",
                                                     "M32R_512KB_1block", image);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(frame({0x81}));
    transport.queue_no_frame();
    transport.expectWrite(frame({0x81}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0xc1});
    scriptAuthenticatedTail(transport);
    scriptWriteBody(transport, image);
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    CancellationSource cancellation;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancellation.token(), events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{4800, 15625}));
}

TEST(SubaruHitachiM32rKlineExecutor, CancellationBeforeSetupPerformsNoIo)
{
    auto plan = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Read, "sub_ecu_hitachi_m32r_kline",
                                                     "M32R_512KB_1block", std::nullopt);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    CancellationSource cancellation;
    cancellation.trip();
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancellation.token(), events);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_FALSE(transport.last_config_.has_value());
}

TEST(SubaruHitachiM32rKlineExecutor, CancellationAfterEraseIsNotReportedAsSuccess)
{
    const bytes::Bytes image(0x80000);
    auto plan = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Write, "sub_ecu_hitachi_m32r_kline",
                                                     "M32R_512KB_1block", image);
    ASSERT_TRUE(plan.has_value());
    CancellationSource cancellation;
    TripOnReadTransport transport(cancellation);
    transport.expectWrite(frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x74, 0x84});
    transport.expectWrite(frame({0x34, 0, 0, 0, 0x04, 0x08, 0, 0}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x74});
    transport.expectWrite(frame({0x31, 0x02, 0x0f, 0xff, 0xff, 0xff}));
    transport.queueRead(bytes::Bytes{0, 0, 0, 0, 0x71, 0x02});
    SubaruHitachiM32rKlineExecutor executor;
    FakeClock clock;
    RecordingEventSink events;
    auto result = executor.execute(*plan, transport, clock, cancellation.token(), events);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.close_call_count_, 1);
}
} // namespace
