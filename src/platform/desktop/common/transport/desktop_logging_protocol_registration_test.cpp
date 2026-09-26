#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <gmock/gmock.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#include "src/platform/desktop/common/logging/logging_worker.h"
#include <QMap>
#include <optional>
// Test the registered factories synchronously without adding a production
// inspection API. Engine/worker lifecycle has its own suite.
#define private public
#include "src/platform/desktop/common/logging/logging_engine.h"
#undef private
#include "src/platform/desktop/common/transport/desktop_logging_protocol_registration.h"
#include "src/platform/desktop/common/transport/fake_backed_serial.h"
#include "src/platform/desktop/common/transport/setter_sequence_expectations.h"

using namespace fastecu::desktop::logging;
using namespace fastecu::logging;
using namespace std::chrono_literals;
using ::testing::_;
using ::testing::Return;

namespace
{
DesktopLoggingSnapshot snapshot(LoggingProtocolId id, std::uint32_t address = 0x804000, std::size_t length = 1)
{
    auto session = make_logging_session(id,
                                        {{.id = "load",
                                          .address = address,
                                          .length = length,
                                          .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
                                          .from_byte_expression = "x",
                                          .unit = "%",
                                          .decimal_precision = 0}},
                                        {.poll_timeout = 50ms,
                                         .car_silence_miss_threshold = 20,
                                         .reconnect_attempt_threshold = 100,
                                         .reconnect_retry_period = 20});
    Q_ASSERT(session);
    return {
        .session = std::move(*session), .response_offsets = {0}, .index_by_id = {{"load", 0}}, .enabled_ids = {"load"}};
}

void expectCdbgSetup(FakeBackend& fake, int failure = 7)
{
    ::testing::InSequence order;
    expectSetterAt(EXPECT_CALL(fake, set_is_iso14230_connection(false)), 0, failure);
    expectSetterAt(EXPECT_CALL(fake, set_add_iso14230_header(false)), 1, failure);
    expectSetterAt(EXPECT_CALL(fake, set_is_can_connection(true)), 2, failure);
    expectSetterAt(EXPECT_CALL(fake, set_is_iso15765_connection(false)), 3, failure);
    expectSetterAt(EXPECT_CALL(fake, set_is_29_bit_id(false)), 4, failure);
    expectSetterAt(EXPECT_CALL(fake, set_can_speed(QStringLiteral("500000"))), 5, failure);
    expectSetterAt(EXPECT_CALL(fake, set_can_destination_address(0x631)), 6, failure);
}

// Complete 51-byte MUT acknowledgement/setup frame, with independently pinned
// checksum/trailer. Only test inputs use this helper.
QByteArray mutFrame(unsigned char command, unsigned char count, unsigned char checksum, unsigned char trailer)
{
    QByteArray frame(51, '\0');
    frame[0] = static_cast<char>(command);
    frame[1] = static_cast<char>(count);
    frame[49] = static_cast<char>(checksum);
    frame[50] = static_cast<char>(trailer);
    return frame;
}
} // namespace

class DesktopLoggingProtocolRegistrationTest : public QObject
{
    Q_OBJECT
  private slots:
    void registration_performs_no_io()
    {
        FakeBackedSerial<::testing::StrictMock<FakeBackend>> serial(
            [](auto& fake) { EXPECT_CALL(fake, set_add_ssm_header(false)).WillOnce(Return(true)); });
        fastecu::FakeClock clock;
        LoggingEngine engine;
        register_desktop_logging_protocols(engine, *serial, clock);
        QCOMPARE(engine.registrations_.keys(), (QStringList{"CDBG", "MUT_DMA", "SSM"}));
    }

    void cdbg_setup_failure_stops_at_failed_step()
    {
        const char *details[] = {"disable ISO 14230 mode",      "disable ISO 14230 header",      "enable raw CAN mode",
                                 "disable ISO 15765 mode",      "select 11-bit CAN identifiers", "select 500000 baud",
                                 "select CDBG reply identifier"};
        for (int failure = 0; failure < 7; ++failure)
        {
            FakeBackedSerial serial;
            fastecu::FakeClock clock;
            LoggingEngine engine;
            register_desktop_logging_protocols(engine, *serial, clock);
            expectCdbgSetup(serial.fake(), failure);
            EXPECT_CALL(serial.fake(), open_serial_port()).Times(0);
            EXPECT_CALL(serial.fake(), is_serial_port_open()).Times(0);
            const auto result = engine.start({.protocolId = "CDBG"}, snapshot(LoggingProtocolId::Cdbg));
            QVERIFY(!result);
            QCOMPARE(result.error().kind, fastecu::ErrorKind::InvalidConfig);
            QCOMPARE(result.error().detail, std::string("failed to ") + details[failure]);
            QVERIFY(!engine.isRunning());
        }
    }

    void cdbg_open_failure()
    {
        for (bool empty : {true, false})
        {
            FakeBackedSerial serial;
            fastecu::FakeClock clock;
            LoggingEngine engine;
            register_desktop_logging_protocols(engine, *serial, clock);
            ::testing::InSequence order;
            expectCdbgSetup(serial.fake());
            EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(empty ? QString{} : QString{"fake"}));
            if (empty)
                EXPECT_CALL(serial.fake(), is_serial_port_open()).Times(0);
            else
                EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(Return(false));
            const auto result = engine.start({.protocolId = "CDBG"}, snapshot(LoggingProtocolId::Cdbg));
            QVERIFY(!result);
            QCOMPARE(result.error().kind, fastecu::ErrorKind::Disconnected);
            QCOMPARE(result.error().detail, std::string("unable to open CAN adapter for CDBG logging"));
        }
    }

    void cdbg_success_preserves_start_sequence()
    {
        FakeBackedSerial serial;
        fastecu::FakeClock clock;
        LoggingEngine engine;
        register_desktop_logging_protocols(engine, *serial, clock);
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillRepeatedly(Return(true));
        // Valid streaming frames prevent the fast fake backend from reaching
        // reconnect thresholds before the GUI thread observes Running.
        EXPECT_CALL(serial.fake(), read_serial_data(50))
            .WillRepeatedly(Return(QByteArray::fromHex("00000631002a000000000000")));
        {
            ::testing::InSequence order;
            expectCdbgSetup(serial.fake());
            EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString{"fake"}));
            const char *requests[] = {"0101000000000000", "1200020000000000", "13008c536b330000", "1400000000000631",
                                      "1500000000000000", "1600010000804000", "060001000100000a"};
            const char *replies[] = {"0000000000000000", "0000000012345678", "0000000100000000", "0000000000000000",
                                     "0000000000000000", "0000000000000000", "0000000000000000"};
            for (int i = 0; i < 7; ++i)
            {
                EXPECT_CALL(serial.fake(), write_serial_data_echo_check(QByteArray::fromHex("00000630") +
                                                                        QByteArray::fromHex(requests[i])))
                    .WillOnce(Return(QByteArray{}));
                EXPECT_CALL(serial.fake(), read_serial_data(250))
                    .WillOnce(Return(QByteArray::fromHex("00000631") + QByteArray::fromHex(replies[i])));
            }
        }
        QSignalSpy status(&engine, &LoggingEngine::statusChanged);
        QVERIFY(engine.start({.protocolId = "CDBG"}, snapshot(LoggingProtocolId::Cdbg)));
        QTRY_VERIFY_WITH_TIMEOUT(!status.isEmpty(), 2000);
        QCOMPARE(status.first().first().value<LoggingStatus>(), LoggingStatus::Running);
        engine.stop();
    }

    void ssm_target_and_adapter_are_per_run()
    {
        FakeBackedSerial serial;
        auto clock = fastecu::make_auto_advancing_clock(10ms);
        LoggingEngine engine;
        register_desktop_logging_protocols(engine, *serial, clock);
        fastecu::FakeCancellationToken cancellation;
        for (bool target : {true, false})
        {
            for (bool openport : {true, false})
            {
                EXPECT_CALL(serial.fake(), is_serial_port_open()).WillRepeatedly(Return(true));
                EXPECT_CALL(serial.fake(), get_use_openport2_adapter()).WillOnce(Return(openport));
                auto data = snapshot(LoggingProtocolId::Ssm, 0x1000);
                data.target_is_ecu = target;
                auto result = engine.registrations_.value("SSM")(data);
                QVERIFY(result);
                EXPECT_CALL(serial.fake(), write_serial_data_echo_check(QByteArray::fromHex(
                                               target ? "8010f005a80000000734" : "8018f005a8000000073c")))
                    .WillOnce(Return(QByteArray{}));
                {
                    ::testing::InSequence order;
                    EXPECT_CALL(serial.fake(), read_serial_data(openport ? 1000 : 10))
                        .WillOnce(Return(QByteArray::fromHex("80f01004e80000006c")));
                    if (!openport)
                        EXPECT_CALL(serial.fake(), read_serial_data(980)).WillOnce(Return(QByteArray{}));
                }
                QVERIFY((*result)->start(cancellation));
                QVERIFY((*result)->stop());
                QVERIFY(::testing::Mock::VerifyAndClearExpectations(&serial.fake()));
            }
        }
    }

    void ssm_snapshot_offsets_reach_samples()
    {
        FakeBackedSerial serial;
        fastecu::FakeClock clock;
        LoggingEngine engine;
        register_desktop_logging_protocols(engine, *serial, clock);
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillRepeatedly(Return(true));
        EXPECT_CALL(serial.fake(), get_use_openport2_adapter()).WillOnce(Return(true));
        auto data = snapshot(LoggingProtocolId::Ssm, 0x1000);
        auto channels = data.session.channels();
        channels.push_back(channels.front());
        channels.back().id = "rpm";
        channels.back().address = 0x1001;
        auto session = make_logging_session(LoggingProtocolId::Ssm, channels, data.session.policy());
        QVERIFY(session);
        data.session = std::move(*session);
        data.response_offsets = {2, 0};
        auto result = engine.registrations_.value("SSM")(data);
        QVERIFY(result);
        EXPECT_CALL(serial.fake(), write_serial_data_echo_check(QByteArray::fromHex("8010f008a80100100000100152")))
            .WillOnce(Return(QByteArray{}));
        EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(Return(QByteArray::fromHex("80f01004e8112233d2")));
        fastecu::FakeCancellationToken cancellation;
        const auto samples = (*result)->poll(50ms, cancellation);
        QVERIFY(samples);
        QVERIFY(samples->responded);
        QCOMPARE(samples->samples.size(), std::size_t{2});
        QCOMPARE(samples->samples[0].channel_id, std::string("load"));
        QCOMPARE(samples->samples[0].raw_value, std::string("51"));
        QCOMPARE(samples->samples[1].channel_id, std::string("rpm"));
        QCOMPARE(samples->samples[1].raw_value, std::string("17"));
    }

    void mut_dma_preserves_initialization_and_channels()
    {
        FakeBackedSerial serial;
        fastecu::FakeClock clock;
        LoggingEngine engine;
        register_desktop_logging_protocols(engine, *serial, clock);
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillRepeatedly(Return(true));
        auto result = engine.registrations_.value("MUT_DMA")(snapshot(LoggingProtocolId::MutDma, 0x8000, 2));
        QVERIFY(result);
        {
            ::testing::InSequence order;
            EXPECT_CALL(serial.fake(), change_port_speed(QStringLiteral("125000"))).WillOnce(Return(0));
            EXPECT_CALL(serial.fake(), write_serial_data(mutFrame(0xa0, 1, 0xa1, 0x0a))).WillOnce(Return(QByteArray{}));
            EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(Return(mutFrame(0xa5, 0, 0xa5, 0x0d)));
            QByteArray ids(31, '\0');
            ids[0] = char(0xa1);
            ids[1] = 1;
            ids[2] = 0x40;
            ids[3] = char(0x80);
            ids[29] = 0x62;
            ids[30] = 0x0d;
            EXPECT_CALL(serial.fake(), write_serial_data(ids)).WillOnce(Return(QByteArray{}));
            EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(Return(mutFrame(5, 0, 5, 0x0d)));
            EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(Return(QByteArray::fromHex("011234470d")));
        }
        fastecu::FakeCancellationToken cancellation;
        QVERIFY((*result)->start(cancellation));
        const auto samples = (*result)->poll(50ms, cancellation);
        QVERIFY(samples);
        QCOMPARE(samples->samples.size(), std::size_t{1});
        QCOMPARE(samples->samples[0].channel_id, std::string("load"));
        QCOMPARE(samples->samples[0].raw_value, std::string("4660"));
        QVERIFY((*result)->stop());
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QCoreApplication app(argc, argv);
    DesktopLoggingProtocolRegistrationTest test;
    const int result = QTest::qExec(&test, argc, argv);
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "desktop_logging_protocol_registration_test.moc"
