#include "src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h"

#include <QTest>

#include <gmock/gmock.h>

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::flash::DesktopMixedCanFlashTransport;
using fastecu::flash::Iso15765Config;
using fastecu::flash::MixedCanConfig;
using fastecu::flash::RawCanConfig;

namespace
{
using namespace std::chrono_literals;

constexpr MixedCanConfig config()
{
    return MixedCanConfig{
        .kernel = Iso15765Config{.bitrate = 500000, .request_id = 2016, .response_id = 2024, .extended_id = false},
        .bootloader = RawCanConfig{.bitrate = 500000, .transmit_id = 1048574, .receive_id = 33, .extended_id = true},
    };
}

std::unique_ptr<SerialPortActions> make_serial(FakeBackend *& fake)
{
    auto serial = std::make_unique<SerialPortActions>(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial->set_add_ssm_header(false);
    EXPECT_CALL(*fake, open_serial_port()).WillRepeatedly(::testing::Return(QStringLiteral("fake-adapter")));
    return serial;
}

void configure_and_open(DesktopMixedCanFlashTransport& transport)
{
    QVERIFY(transport.configure(config()).has_value());
    QVERIFY(transport.open().has_value());
}

} // namespace

class TestDesktopMixedCanFlashTransport : public QObject
{
    Q_OBJECT

  private slots:
    void initialResetReachesBackendAndReturnsFailure()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        EXPECT_CALL(*fake, reset_connection()).WillOnce(::testing::Return());

        QVERIFY(transport.reset_connection().has_value());

        EXPECT_CALL(*fake, reset_connection())
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend reset failure")));
        const auto failed = transport.reset_connection();
        QVERIFY(!failed.has_value());
        QCOMPARE(failed.error().kind, ErrorKind::Internal);
    }

    void initialResetAfterCloseReturnsDisconnected()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        QVERIFY(transport.close().has_value());

        const auto result = transport.reset_connection();

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    void configuresIsoThenTransitionsRawAndBack()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));

        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, set_is_iso14230_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_can_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_iso15765_connection(true)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_29_bit_id(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_can_speed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_can_source_address(1048574)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_can_destination_address(33)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_iso15765_source_address(2016)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_iso15765_destination_address(2024)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, open_serial_port()).WillOnce(::testing::Return(QStringLiteral("fake-adapter")));
        EXPECT_CALL(*fake, reset_connection()).WillOnce(::testing::Return());
        EXPECT_CALL(*fake, set_is_iso14230_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_can_connection(true)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_iso15765_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_29_bit_id(true)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_can_speed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_can_source_address(1048574)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_can_destination_address(33)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, open_serial_port()).WillOnce(::testing::Return(QStringLiteral("fake-adapter")));
        EXPECT_CALL(*fake, reset_connection()).WillOnce(::testing::Return());
        EXPECT_CALL(*fake, set_is_iso14230_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_can_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_iso15765_connection(true)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_29_bit_id(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_can_speed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_can_source_address(1048574)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_can_destination_address(33)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_iso15765_source_address(2016)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_iso15765_destination_address(2024)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, open_serial_port()).WillOnce(::testing::Return(QStringLiteral("fake-adapter")));

        QVERIFY(transport.configure(config()).has_value());
        QVERIFY(transport.open().has_value());
        QVERIFY(transport.enter_raw_bootloader_mode().has_value());
        QVERIFY(transport.enter_iso15765_kernel_mode().has_value());
    }

    void everyModeConfigurationClearsStickyIso14230HeaderState()
    {
        FakeBackend *fake = nullptr;
        auto serial = make_serial(fake);
        SerialPortActions *observed = serial.get();
        QVERIFY(observed->set_add_iso14230_header(true));
        DesktopMixedCanFlashTransport transport(std::move(serial));

        QVERIFY(transport.configure(config()).has_value());
        QCOMPARE(observed->get_add_iso14230_header(), false);
        QVERIFY(transport.open().has_value());

        QVERIFY(observed->set_add_iso14230_header(true));
        QVERIFY(transport.enter_raw_bootloader_mode().has_value());
        QCOMPARE(observed->get_add_iso14230_header(), false);

        QVERIFY(observed->set_add_iso14230_header(true));
        QVERIFY(transport.enter_iso15765_kernel_mode().has_value());
        QCOMPARE(observed->get_add_iso14230_header(), false);
    }

    void preservesExtendedIsoIdDuringInitialConfigurationAndReturnTransition()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        MixedCanConfig extended = config();
        extended.kernel.extended_id = true;
        EXPECT_CALL(*fake, set_is_29_bit_id(true)).Times(3);

        QVERIFY(transport.configure(extended).has_value());
        QVERIFY(transport.open().has_value());
        QVERIFY(transport.enter_raw_bootloader_mode().has_value());
        QVERIFY(transport.enter_iso15765_kernel_mode().has_value());
    }

    void rawFrameAddsAndParsesBigEndianId()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        FakeCancellationToken cancellation;
        configure_and_open(transport);
        QVERIFY(transport.enter_raw_bootloader_mode().has_value());

        const QByteArray expectedWrite = QByteArray::fromHex("000ffffe7a90000000000000");
        EXPECT_CALL(*fake, write_serial_data_echo_check(expectedWrite)).WillOnce(::testing::Return(QByteArray{}));
        QVERIFY(transport.write_raw({0x000ffffe, {0x7a, 0x90, 0, 0, 0, 0, 0, 0}}, cancellation).has_value());

        EXPECT_CALL(*fake, read_serial_data(::testing::_))
            .WillOnce(::testing::Return(QByteArray::fromHex("000000217a96000000000000")));
        const auto frame = transport.read_raw(800ms, cancellation);
        QVERIFY(frame.has_value());
        QVERIFY(frame->has_value());
        QCOMPARE(frame->value().id, 0x21U);
        QCOMPARE(frame->value().payload, (bytes::Bytes{0x7a, 0x96, 0, 0, 0, 0, 0, 0}));
    }

    void configureFailsAtEverySetter()
    {
        const std::array<std::function<void(FakeBackend&)>, 10> failures{
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_is_iso14230_connection(false)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_is_can_connection(false)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_is_iso15765_connection(true)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake) { EXPECT_CALL(fake, set_is_29_bit_id(false)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_can_speed(::testing::_)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_can_source_address(::testing::_)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_can_destination_address(::testing::_)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_iso15765_source_address(::testing::_)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_iso15765_destination_address(::testing::_)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(false)); },
        };
        for (const auto& set_failure : failures)
        {
            FakeBackend *fake = nullptr;
            DesktopMixedCanFlashTransport transport(make_serial(fake));
            set_failure(*fake);
            const auto result = transport.configure(config());
            QVERIFY(!result.has_value());
            QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
        }
    }

    void configureRejectsReconfigureWhileAlreadyConfigured()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        configure_and_open(transport);

        const auto reconfigure = transport.configure(config());

        QVERIFY(!reconfigure.has_value());
        QCOMPARE(reconfigure.error().kind, ErrorKind::InvalidConfig);
    }

    void poisonedTransitionMakesConfigureAndOpenSurfaceTheStaleErrorEvenAfterClose()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        configure_and_open(transport);
        EXPECT_CALL(*fake, open_serial_port()).WillOnce(::testing::Return(QString{}));

        const auto transition = transport.enter_raw_bootloader_mode();
        QVERIFY(!transition.has_value());
        QCOMPARE(transition.error().kind, ErrorKind::Disconnected);

        // close() must not clear the poison: it only tears down the live handle.
        QVERIFY(transport.close().has_value());

        const auto reconfigure = transport.configure(config());
        QVERIFY(!reconfigure.has_value());
        QCOMPARE(reconfigure.error().kind, ErrorKind::Disconnected);

        const auto reopen = transport.open();
        QVERIFY(!reopen.has_value());
        QCOMPARE(reopen.error().kind, ErrorKind::Disconnected);
    }

    void rawTransitionFailsAtEverySetterAndMakesIoTerminal()
    {
        const std::array<std::function<void(FakeBackend&)>, 8> failures{
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_is_iso14230_connection(false)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_is_can_connection(true)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_is_iso15765_connection(false)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake) { EXPECT_CALL(fake, set_is_29_bit_id(true)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_can_speed(::testing::_)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_can_source_address(::testing::_)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_can_destination_address(::testing::_)).WillOnce(::testing::Return(false)); },
            [](FakeBackend& fake)
            { EXPECT_CALL(fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(false)); },
        };
        for (const auto& set_failure : failures)
        {
            FakeBackend *fake = nullptr;
            DesktopMixedCanFlashTransport transport(make_serial(fake));
            FakeCancellationToken cancellation;
            configure_and_open(transport);
            set_failure(*fake);

            const auto transition = transport.enter_raw_bootloader_mode();
            QVERIFY(!transition.has_value());
            QCOMPARE(transition.error().kind, ErrorKind::InvalidConfig);
            EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
            const auto write = transport.write_iso15765(bytes::Bytes{0x01}, cancellation);
            QVERIFY(!write.has_value());
            QCOMPARE(write.error().kind, ErrorKind::InvalidConfig);
        }
    }

    void failedReopenMakesFollowingIoTerminal()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        FakeCancellationToken cancellation;
        configure_and_open(transport);
        EXPECT_CALL(*fake, open_serial_port()).WillOnce(::testing::Return(QString{}));

        const auto transition = transport.enter_raw_bootloader_mode();
        QVERIFY(!transition.has_value());
        QCOMPARE(transition.error().kind, ErrorKind::Disconnected);
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
        const auto write = transport.write_raw({0x000ffffe, {0x7a}}, cancellation);
        QVERIFY(!write.has_value());
        QCOMPARE(write.error().kind, ErrorKind::Disconnected);
    }

    void rawReadRejectsShortFrameAndWrongReceiveId()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        FakeCancellationToken cancellation;
        configure_and_open(transport);
        QVERIFY(transport.enter_raw_bootloader_mode().has_value());

        EXPECT_CALL(*fake, read_serial_data(::testing::_)).WillOnce(::testing::Return(QByteArray::fromHex("000021")));
        const auto short_frame = transport.read_raw(10ms, cancellation);
        QVERIFY(!short_frame.has_value());
        QCOMPARE(short_frame.error().kind, ErrorKind::BadResponse);

        EXPECT_CALL(*fake, read_serial_data(::testing::_))
            .WillOnce(::testing::Return(QByteArray::fromHex("000000227a96")));
        const auto wrong_id = transport.read_raw(10ms, cancellation);
        QVERIFY(!wrong_id.has_value());
        QCOMPARE(wrong_id.error().kind, ErrorKind::BadResponse);
    }

    void clearReceiveBufferRejectsBackendFailure()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        configure_and_open(transport);
        QVERIFY(transport.enter_raw_bootloader_mode().has_value());
        EXPECT_CALL(*fake, clear_rx_buffer()).WillOnce(::testing::Return(STATUS_ERROR));

        const auto result = transport.clear_receive_buffer();
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    void detectsDisconnectionBeforeAndDuringIo()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        FakeCancellationToken cancellation;
        configure_and_open(transport);

        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
        const auto before_write = transport.write_iso15765(bytes::Bytes{0x01}, cancellation);
        QVERIFY(!before_write.has_value());
        QCOMPARE(before_write.error().kind, ErrorKind::Disconnected);

        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).WillOnce(::testing::Return(QByteArray("\x01", 1)));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
        const auto during_read = transport.read_iso15765(10ms, cancellation);
        QVERIFY(!during_read.has_value());
        QCOMPARE(during_read.error().kind, ErrorKind::Disconnected);
    }

    void catchesStandardAndNonstandardBackendExceptions()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        FakeCancellationToken cancellation;
        configure_and_open(transport);

        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend write failure")));
        const auto standard = transport.write_iso15765(bytes::Bytes{0x01}, cancellation);
        QVERIFY(!standard.has_value());
        QCOMPARE(standard.error().kind, ErrorKind::Internal);

        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());
        const auto nonstandard = transport.write_iso15765(bytes::Bytes{0x01}, cancellation);
        QVERIFY(!nonstandard.has_value());
        QCOMPARE(nonstandard.error().kind, ErrorKind::Internal);
    }

    void cancellationAndUnblockSuppressSubsequentIo()
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        FakeCancellationToken cancelled(true);
        configure_and_open(transport);
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);

        const auto cancelled_write = transport.write_iso15765(bytes::Bytes{0x01}, cancelled);
        QVERIFY(!cancelled_write.has_value());
        QCOMPARE(cancelled_write.error().kind, ErrorKind::Cancelled);

        FakeCancellationToken cancellation;
        transport.request_unblock();
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
        const auto unblocked_read = transport.read_iso15765(10ms, cancellation);
        QVERIFY(!unblocked_read.has_value());
        QCOMPARE(unblocked_read.error().kind, ErrorKind::Cancelled);
    }

    void nonOwningCloseDoesNotDestroyCallerSerial()
    {
        FakeBackend *fake = nullptr;
        auto serial = make_serial(fake);
        bool destroyed = false;
        fake->destroyed = &destroyed;

        DesktopMixedCanFlashTransport transport(serial.get());
        QVERIFY(transport.close().has_value());
        QVERIFY(!destroyed);
        const bool still_callable = serial->is_serial_port_open();
        Q_UNUSED(still_callable);
        serial.reset();
        QVERIFY(destroyed);
    }
};

QTEST_GUILESS_MAIN(TestDesktopMixedCanFlashTransport)
#include "desktop_mixed_can_flash_transport_test.moc"
