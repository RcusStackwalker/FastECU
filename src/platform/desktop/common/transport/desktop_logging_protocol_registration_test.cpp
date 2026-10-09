#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <QCoreApplication>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#include "src/platform/desktop/common/logging/runtime/logging_worker.h"
#include <QMap>
#include <array>
#include <optional>
// Test the registered factories synchronously without adding a production
// inspection API. Engine/worker lifecycle has its own suite.
#define private public
#include "src/platform/desktop/common/logging/runtime/logging_engine.h"
#undef private
#include "src/platform/desktop/common/transport/desktop_logging_protocol_registration.h"
#include "src/platform/desktop/common/serial/testing/fake_backed_serial.h"
#include "src/platform/desktop/common/transport/setter_sequence_expectations.h"

using namespace fastecu::desktop::logging;
using namespace fastecu::logging;
using namespace std::chrono_literals;
using ::testing::_;
using ::testing::Return;

namespace
{
DesktopLoggingSnapshot Snapshot(LoggingProtocolId id, std::uint32_t address = 0x804000, std::size_t length = 1)
{
    auto session = MakeLoggingSession(id,
                                      {{.id = "load",
                                        .address = address,
                                        .length = length,
                                        .raw_assembly = RawAssembly::kUnsignedIntegerDecimal,
                                        .from_byte_expression = "x",
                                        .unit = "%",
                                        .decimal_precision = 0}},
                                      {.poll_timeout = 50ms,
                                       .car_silence_miss_threshold = 20,
                                       .reconnect_attempt_threshold = 100,
                                       .reconnect_retry_period = 20});
    Q_ASSERT(session);
    return {.session = std::move(*session),
            .response_offsets = {0},
            .protocol = "SSM",
            .identities_by_id = {{"load", {"SSM", "load"}}},
            .enabled_ids = {"load"}};
}

void ExpectCdbgSetup(FakeBackend& fake, int failure = 7)
{
    ::testing::InSequence order;
    ExpectSetterAt(EXPECT_CALL(fake, SetIsIso14230Connection(false)), 0, failure);
    ExpectSetterAt(EXPECT_CALL(fake, SetAddIso14230Header(false)), 1, failure);
    ExpectSetterAt(EXPECT_CALL(fake, SetIsCanConnection(true)), 2, failure);
    ExpectSetterAt(EXPECT_CALL(fake, SetIsIso15765Connection(false)), 3, failure);
    ExpectSetterAt(EXPECT_CALL(fake, SetIs29BitId(false)), 4, failure);
    ExpectSetterAt(EXPECT_CALL(fake, SetCanSpeed(QStringLiteral("500000"))), 5, failure);
    ExpectSetterAt(EXPECT_CALL(fake, SetCanDestinationAddress(0x631)), 6, failure);
}

// Complete 51-byte MUT acknowledgement/setup frame, with independently pinned
// checksum/trailer. Only test inputs use this helper.
QByteArray MutFrame(unsigned char command, unsigned char count, unsigned char checksum, unsigned char trailer)
{
    QByteArray frame(51, '\0');
    frame[0] = static_cast<char>(command);
    frame[1] = static_cast<char>(count);
    frame[49] = static_cast<char>(checksum);
    frame[50] = static_cast<char>(trailer);
    return frame;
}
} // namespace

TEST(DesktopLoggingProtocolRegistrationTest, registration_performs_no_io)
{
    FakeBackedSerial<::testing::StrictMock<FakeBackend>> serial(
        [](auto& fake) { EXPECT_CALL(fake, SetAddSsmHeader(false)).WillOnce(Return(true)); });
    fastecu::FakeClock clock;
    LoggingEngine engine;
    RegisterDesktopLoggingProtocols(engine, *serial, clock);
    ASSERT_EQ(engine.registrations_.keys(), (QStringList{"CDBG", "MUT_DMA", "SSM"}));
}

TEST(DesktopLoggingProtocolRegistrationTest, cdbg_setup_failure_stops_at_failed_step)
{
    const std::array<const char *, 7> details = {"disable ISO 14230 mode",        "disable ISO 14230 header",
                                                 "enable raw CAN mode",           "disable ISO 15765 mode",
                                                 "select 11-bit CAN identifiers", "select 500000 baud",
                                                 "select CDBG reply identifier"};
    for (int failure = 0; failure < 7; ++failure)
    {
        FakeBackedSerial serial;
        fastecu::FakeClock clock;
        LoggingEngine engine;
        RegisterDesktopLoggingProtocols(engine, *serial, clock);
        ExpectCdbgSetup(serial.Fake(), failure);
        EXPECT_CALL(serial.Fake(), OpenSerialPort()).Times(0);
        EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).Times(0);
        const auto result = engine.Start({.protocol_id = "CDBG"}, Snapshot(LoggingProtocolId::kCdbg));
        ASSERT_TRUE(!result);
        ASSERT_EQ(result.error().kind, fastecu::ErrorKind::kInvalidConfig);
        ASSERT_EQ(result.error().detail, std::string("failed to ") + details[failure]);
        ASSERT_TRUE(!engine.IsRunning());
    }
}

TEST(DesktopLoggingProtocolRegistrationTest, cdbg_open_failure)
{
    for (bool empty : {true, false})
    {
        FakeBackedSerial serial;
        fastecu::FakeClock clock;
        LoggingEngine engine;
        RegisterDesktopLoggingProtocols(engine, *serial, clock);
        ::testing::InSequence order;
        ExpectCdbgSetup(serial.Fake());
        EXPECT_CALL(serial.Fake(), OpenSerialPort()).WillOnce(Return(empty ? QString{} : QString{"fake"}));
        if (empty)
        {
            EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).Times(0);
        }
        else
        {
            EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(Return(false));
        }
        const auto result = engine.Start({.protocol_id = "CDBG"}, Snapshot(LoggingProtocolId::kCdbg));
        ASSERT_TRUE(!result);
        ASSERT_EQ(result.error().kind, fastecu::ErrorKind::kDisconnected);
        ASSERT_EQ(result.error().detail, std::string("unable to open CAN adapter for CDBG logging"));
    }
}

TEST(DesktopLoggingProtocolRegistrationTest, cdbg_success_preserves_start_sequence)
{
    FakeBackedSerial serial;
    fastecu::FakeClock clock;
    LoggingEngine engine;
    RegisterDesktopLoggingProtocols(engine, *serial, clock);
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillRepeatedly(Return(true));
    // Valid streaming frames prevent the fast fake backend from reaching
    // reconnect thresholds before the GUI thread observes Running.
    EXPECT_CALL(serial.Fake(), ReadSerialData(50))
        .WillRepeatedly(Return(QByteArray::fromHex("00000631002a000000000000")));
    {
        ::testing::InSequence order;
        ExpectCdbgSetup(serial.Fake());
        EXPECT_CALL(serial.Fake(), OpenSerialPort()).WillOnce(Return(QString{"fake"}));
        const std::array<const char *, 7> requests = {"0101000000000000", "1200020000000000", "13008c536b330000",
                                                      "1400000000000631", "1500000000000000", "1600010000804000",
                                                      "060001000100000a"};
        const std::array<const char *, 7> replies = {"0000000000000000", "0000000012345678", "0000000100000000",
                                                     "0000000000000000", "0000000000000000", "0000000000000000",
                                                     "0000000000000000"};
        for (int i = 0; i < 7; ++i)
        {
            EXPECT_CALL(serial.Fake(),
                        WriteSerialDataEchoCheck(QByteArray::fromHex("00000630") + QByteArray::fromHex(requests[i])))
                .WillOnce(Return(QByteArray{}));
            EXPECT_CALL(serial.Fake(), ReadSerialData(250))
                .WillOnce(Return(QByteArray::fromHex("00000631") + QByteArray::fromHex(replies[i])));
        }
    }
    fastecu::testing::SignalRecorder status(&engine, &LoggingEngine::statusChanged);
    ASSERT_TRUE(engine.Start({.protocol_id = "CDBG"}, Snapshot(LoggingProtocolId::kCdbg)));
    ASSERT_TRUE(
        fastecu::testing::WaitUntil([&] { return !status.Snapshot().empty(); }, std::chrono::milliseconds(2000)));
    ASSERT_EQ(std::get<0>(status.Snapshot().front()), LoggingStatus::kRunning);
    engine.Stop();
}

TEST(DesktopLoggingProtocolRegistrationTest, ssm_target_and_adapter_are_per_run)
{
    FakeBackedSerial serial;
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    LoggingEngine engine;
    RegisterDesktopLoggingProtocols(engine, *serial, clock);
    fastecu::FakeCancellationToken cancellation;
    for (bool target : {true, false})
    {
        for (bool openport : {true, false})
        {
            EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillRepeatedly(Return(true));
            EXPECT_CALL(serial.Fake(), GetUseOpenport2Adapter()).WillOnce(Return(openport));
            auto data = Snapshot(LoggingProtocolId::kSsm, 0x1000);
            data.target_is_ecu = target;
            auto result = engine.registrations_.value("SSM")(data);
            ASSERT_TRUE(result);
            EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(QByteArray::fromHex(target ? "8010f005a80000000734"
                                                                                           : "8018f005a8000000073c")))
                .WillOnce(Return(QByteArray{}));
            {
                ::testing::InSequence order;
                EXPECT_CALL(serial.Fake(), ReadSerialData(openport ? 1000 : 10))
                    .WillOnce(Return(QByteArray::fromHex("80f01004e80000006c")));
                if (!openport)
                {
                    EXPECT_CALL(serial.Fake(), ReadSerialData(980)).WillOnce(Return(QByteArray{}));
                }
            }
            ASSERT_TRUE((*result)->Start(cancellation));
            ASSERT_TRUE((*result)->Stop());
            ASSERT_TRUE(::testing::Mock::VerifyAndClearExpectations(&serial.Fake()));
        }
    }
}

TEST(DesktopLoggingProtocolRegistrationTest, ssm_snapshot_offsets_reach_samples)
{
    FakeBackedSerial serial;
    fastecu::FakeClock clock;
    LoggingEngine engine;
    RegisterDesktopLoggingProtocols(engine, *serial, clock);
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillRepeatedly(Return(true));
    EXPECT_CALL(serial.Fake(), GetUseOpenport2Adapter()).WillOnce(Return(true));
    auto data = Snapshot(LoggingProtocolId::kSsm, 0x1000);
    auto channels = data.session.Channels();
    channels.push_back(channels.front());
    channels.back().id = "rpm";
    channels.back().address = 0x1001;
    auto session = MakeLoggingSession(LoggingProtocolId::kSsm, channels, data.session.Policy());
    ASSERT_TRUE(session);
    data.session = std::move(*session);
    data.response_offsets = {2, 0};
    auto result = engine.registrations_.value("SSM")(data);
    ASSERT_TRUE(result);
    EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(QByteArray::fromHex("8010f008a80100100000100152")))
        .WillOnce(Return(QByteArray{}));
    EXPECT_CALL(serial.Fake(), ReadSerialData(50)).WillOnce(Return(QByteArray::fromHex("80f01004e8112233d2")));
    fastecu::FakeCancellationToken cancellation;
    const auto samples = (*result)->Poll(50ms, cancellation);
    ASSERT_TRUE(samples);
    ASSERT_TRUE(samples->responded);
    ASSERT_EQ(samples->samples.size(), std::size_t{2});
    ASSERT_EQ(samples->samples[0].channel_id, std::string("load"));
    ASSERT_EQ(samples->samples[0].raw_value, std::string("51"));
    ASSERT_EQ(samples->samples[1].channel_id, std::string("rpm"));
    ASSERT_EQ(samples->samples[1].raw_value, std::string("17"));
}

TEST(DesktopLoggingProtocolRegistrationTest, mut_dma_preserves_initialization_and_channels)
{
    FakeBackedSerial serial;
    fastecu::FakeClock clock;
    LoggingEngine engine;
    RegisterDesktopLoggingProtocols(engine, *serial, clock);
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillRepeatedly(Return(true));
    auto result = engine.registrations_.value("MUT_DMA")(Snapshot(LoggingProtocolId::kMutDma, 0x8000, 2));
    ASSERT_TRUE(result);
    {
        ::testing::InSequence order;
        EXPECT_CALL(serial.Fake(), ChangePortSpeed(QStringLiteral("125000"))).WillOnce(Return(0));
        EXPECT_CALL(serial.Fake(), WriteSerialData(MutFrame(0xa0, 1, 0xa1, 0x0a))).WillOnce(Return(QByteArray{}));
        EXPECT_CALL(serial.Fake(), ReadSerialData(50)).WillOnce(Return(MutFrame(0xa5, 0, 0xa5, 0x0d)));
        QByteArray ids(31, '\0');
        ids[0] = char(0xa1);
        ids[1] = 1;
        ids[2] = 0x40;
        ids[4] = char(0x80);
        ids[29] = 0x62;
        ids[30] = 0x0d;
        EXPECT_CALL(serial.Fake(), WriteSerialData(ids)).WillOnce(Return(QByteArray{}));
        EXPECT_CALL(serial.Fake(), ReadSerialData(50)).WillOnce(Return(MutFrame(5, 0, 5, 0x0d)));
        EXPECT_CALL(serial.Fake(), ReadSerialData(50)).WillOnce(Return(QByteArray::fromHex("013412470d")));
    }
    fastecu::FakeCancellationToken cancellation;
    ASSERT_TRUE((*result)->Start(cancellation));
    const auto samples = (*result)->Poll(50ms, cancellation);
    ASSERT_TRUE(samples);
    ASSERT_EQ(samples->samples.size(), std::size_t{1});
    ASSERT_EQ(samples->samples[0].channel_id, std::string("load"));
    ASSERT_EQ(samples->samples[0].raw_value, std::string("4660"));
    ASSERT_TRUE((*result)->Stop());
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
