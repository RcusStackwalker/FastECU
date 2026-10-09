#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h"

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
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
    ASSERT_TRUE(transport.configure(config()).has_value());
    ASSERT_TRUE(transport.open().has_value());
}

} // namespace

TEST(TestDesktopMixedCanFlashTransport, initialResetReachesBackendAndReturnsFailure)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    EXPECT_CALL(*fake, reset_connection()).WillOnce(::testing::Return());

    ASSERT_TRUE(transport.reset_connection().has_value());

    EXPECT_CALL(*fake, reset_connection())
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend reset failure")));
    const auto failed = transport.reset_connection();
    ASSERT_TRUE(!failed.has_value());
    ASSERT_EQ(failed.error().kind, ErrorKind::kInternal);
}

TEST(TestDesktopMixedCanFlashTransport, initialResetAfterCloseReturnsDisconnected)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    ASSERT_TRUE(transport.close().has_value());

    const auto result = transport.reset_connection();

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

TEST(TestDesktopMixedCanFlashTransport, configuresIsoThenTransitionsRawAndBack)
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

    ASSERT_TRUE(transport.configure(config()).has_value());
    ASSERT_TRUE(transport.open().has_value());
    ASSERT_TRUE(transport.enter_raw_bootloader_mode().has_value());
    ASSERT_TRUE(transport.enter_iso15765_kernel_mode().has_value());
}

TEST(TestDesktopMixedCanFlashTransport, everyModeConfigurationClearsStickyIso14230HeaderState)
{
    FakeBackend *fake = nullptr;
    auto serial = make_serial(fake);
    SerialPortActions *observed = serial.get();
    ASSERT_TRUE(observed->set_add_iso14230_header(true));
    DesktopMixedCanFlashTransport transport(std::move(serial));

    ASSERT_TRUE(transport.configure(config()).has_value());
    ASSERT_EQ(observed->get_add_iso14230_header(), false);
    ASSERT_TRUE(transport.open().has_value());

    ASSERT_TRUE(observed->set_add_iso14230_header(true));
    ASSERT_TRUE(transport.enter_raw_bootloader_mode().has_value());
    ASSERT_EQ(observed->get_add_iso14230_header(), false);

    ASSERT_TRUE(observed->set_add_iso14230_header(true));
    ASSERT_TRUE(transport.enter_iso15765_kernel_mode().has_value());
    ASSERT_EQ(observed->get_add_iso14230_header(), false);
}

TEST(TestDesktopMixedCanFlashTransport, preservesExtendedIsoIdDuringInitialConfigurationAndReturnTransition)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    MixedCanConfig extended = config();
    extended.kernel.extended_id = true;
    EXPECT_CALL(*fake, set_is_29_bit_id(true)).Times(3);

    ASSERT_TRUE(transport.configure(extended).has_value());
    ASSERT_TRUE(transport.open().has_value());
    ASSERT_TRUE(transport.enter_raw_bootloader_mode().has_value());
    ASSERT_TRUE(transport.enter_iso15765_kernel_mode().has_value());
}

TEST(TestDesktopMixedCanFlashTransport, rawFrameAddsAndParsesBigEndianId)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(configure_and_open(transport));
    ASSERT_TRUE(transport.enter_raw_bootloader_mode().has_value());

    const QByteArray expectedWrite = QByteArray::fromHex("000ffffe7a90000000000000");
    EXPECT_CALL(*fake, write_serial_data_echo_check(expectedWrite)).WillOnce(::testing::Return(QByteArray{}));
    ASSERT_TRUE(transport.write_raw({0x000ffffe, {0x7a, 0x90, 0, 0, 0, 0, 0, 0}}, cancellation).has_value());

    EXPECT_CALL(*fake, read_serial_data(::testing::_))
        .WillOnce(::testing::Return(QByteArray::fromHex("000000217a96000000000000")));
    const auto frame = transport.read_raw(800ms, cancellation);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(frame->has_value());
    ASSERT_EQ(frame->value().id, 0x21U);
    ASSERT_EQ(frame->value().payload, (bytes::Bytes{0x7a, 0x96, 0, 0, 0, 0, 0, 0}));
}

TEST(TestDesktopMixedCanFlashTransport, configureFailsAtEverySetter)
{
    const std::array<std::function<void(FakeBackend&)>, 10> failures{
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, set_is_iso14230_connection(false)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, set_is_can_connection(false)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, set_is_iso15765_connection(true)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, set_is_29_bit_id(false)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, set_can_speed(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, set_can_source_address(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, set_can_destination_address(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, set_iso15765_source_address(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, set_iso15765_destination_address(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(false)); },
    };
    for (const auto& set_failure : failures)
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        set_failure(*fake);
        const auto result = transport.configure(config());
        ASSERT_TRUE(!result.has_value());
        ASSERT_EQ(result.error().kind, ErrorKind::kInvalidConfig);
    }
}

TEST(TestDesktopMixedCanFlashTransport, configureRejectsReconfigureWhileAlreadyConfigured)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    ASSERT_NO_FATAL_FAILURE(configure_and_open(transport));

    const auto reconfigure = transport.configure(config());

    ASSERT_TRUE(!reconfigure.has_value());
    ASSERT_EQ(reconfigure.error().kind, ErrorKind::kInvalidConfig);
}

TEST(TestDesktopMixedCanFlashTransport, poisonedTransitionMakesConfigureAndOpenSurfaceTheStaleErrorEvenAfterClose)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    ASSERT_NO_FATAL_FAILURE(configure_and_open(transport));
    EXPECT_CALL(*fake, open_serial_port()).WillOnce(::testing::Return(QString{}));

    const auto transition = transport.enter_raw_bootloader_mode();
    ASSERT_TRUE(!transition.has_value());
    ASSERT_EQ(transition.error().kind, ErrorKind::kDisconnected);

    // close() must not clear the poison: it only tears down the live handle.
    ASSERT_TRUE(transport.close().has_value());

    const auto reconfigure = transport.configure(config());
    ASSERT_TRUE(!reconfigure.has_value());
    ASSERT_EQ(reconfigure.error().kind, ErrorKind::kDisconnected);

    const auto reopen = transport.open();
    ASSERT_TRUE(!reopen.has_value());
    ASSERT_EQ(reopen.error().kind, ErrorKind::kDisconnected);
}

TEST(TestDesktopMixedCanFlashTransport, rawTransitionFailsAtEverySetterAndMakesIoTerminal)
{
    const std::array<std::function<void(FakeBackend&)>, 8> failures{
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, set_is_iso14230_connection(false)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, set_is_can_connection(true)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, set_is_iso15765_connection(false)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, set_is_29_bit_id(true)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, set_can_speed(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, set_can_source_address(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, set_can_destination_address(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(false)); },
    };
    for (const auto& set_failure : failures)
    {
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(make_serial(fake));
        FakeCancellationToken cancellation;
        ASSERT_NO_FATAL_FAILURE(configure_and_open(transport));
        set_failure(*fake);

        const auto transition = transport.enter_raw_bootloader_mode();
        ASSERT_TRUE(!transition.has_value());
        ASSERT_EQ(transition.error().kind, ErrorKind::kInvalidConfig);
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
        const auto write = transport.write_iso15765(bytes::Bytes{0x01}, cancellation);
        ASSERT_TRUE(!write.has_value());
        ASSERT_EQ(write.error().kind, ErrorKind::kInvalidConfig);
    }
}

TEST(TestDesktopMixedCanFlashTransport, failedReopenMakesFollowingIoTerminal)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(configure_and_open(transport));
    EXPECT_CALL(*fake, open_serial_port()).WillOnce(::testing::Return(QString{}));

    const auto transition = transport.enter_raw_bootloader_mode();
    ASSERT_TRUE(!transition.has_value());
    ASSERT_EQ(transition.error().kind, ErrorKind::kDisconnected);
    EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
    const auto write = transport.write_raw({0x000ffffe, {0x7a}}, cancellation);
    ASSERT_TRUE(!write.has_value());
    ASSERT_EQ(write.error().kind, ErrorKind::kDisconnected);
}

TEST(TestDesktopMixedCanFlashTransport, rawReadRejectsShortFrameAndWrongReceiveId)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(configure_and_open(transport));
    ASSERT_TRUE(transport.enter_raw_bootloader_mode().has_value());

    EXPECT_CALL(*fake, read_serial_data(::testing::_)).WillOnce(::testing::Return(QByteArray::fromHex("000021")));
    const auto short_frame = transport.read_raw(10ms, cancellation);
    ASSERT_TRUE(!short_frame.has_value());
    ASSERT_EQ(short_frame.error().kind, ErrorKind::kBadResponse);

    EXPECT_CALL(*fake, read_serial_data(::testing::_)).WillOnce(::testing::Return(QByteArray::fromHex("000000227a96")));
    const auto wrong_id = transport.read_raw(10ms, cancellation);
    ASSERT_TRUE(!wrong_id.has_value());
    ASSERT_EQ(wrong_id.error().kind, ErrorKind::kBadResponse);
}

TEST(TestDesktopMixedCanFlashTransport, clearReceiveBufferRejectsBackendFailure)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    ASSERT_NO_FATAL_FAILURE(configure_and_open(transport));
    ASSERT_TRUE(transport.enter_raw_bootloader_mode().has_value());
    EXPECT_CALL(*fake, clear_rx_buffer()).WillOnce(::testing::Return(kSerialError));

    const auto result = transport.clear_receive_buffer();
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

TEST(TestDesktopMixedCanFlashTransport, detectsDisconnectionBeforeAndDuringIo)
{
    ::testing::InSequence sequence;
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(configure_and_open(transport));

    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
    const auto before_write = transport.write_iso15765(bytes::Bytes{0x01}, cancellation);
    ASSERT_TRUE(!before_write.has_value());
    ASSERT_EQ(before_write.error().kind, ErrorKind::kDisconnected);

    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).WillOnce(::testing::Return(QByteArray("\x01", 1)));
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
    const auto during_read = transport.read_iso15765(10ms, cancellation);
    ASSERT_TRUE(!during_read.has_value());
    ASSERT_EQ(during_read.error().kind, ErrorKind::kDisconnected);
}

TEST(TestDesktopMixedCanFlashTransport, catchesStandardAndNonstandardBackendExceptions)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(configure_and_open(transport));

    EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend write failure")));
    const auto standard = transport.write_iso15765(bytes::Bytes{0x01}, cancellation);
    ASSERT_TRUE(!standard.has_value());
    ASSERT_EQ(standard.error().kind, ErrorKind::kInternal);

    EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());
    const auto nonstandard = transport.write_iso15765(bytes::Bytes{0x01}, cancellation);
    ASSERT_TRUE(!nonstandard.has_value());
    ASSERT_EQ(nonstandard.error().kind, ErrorKind::kInternal);
}

TEST(TestDesktopMixedCanFlashTransport, cancellationAndUnblockSuppressSubsequentIo)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(make_serial(fake));
    FakeCancellationToken cancelled(true);
    ASSERT_NO_FATAL_FAILURE(configure_and_open(transport));
    EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);

    const auto cancelled_write = transport.write_iso15765(bytes::Bytes{0x01}, cancelled);
    ASSERT_TRUE(!cancelled_write.has_value());
    ASSERT_EQ(cancelled_write.error().kind, ErrorKind::kCancelled);

    FakeCancellationToken cancellation;
    transport.request_unblock();
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
    const auto unblocked_read = transport.read_iso15765(10ms, cancellation);
    ASSERT_TRUE(!unblocked_read.has_value());
    ASSERT_EQ(unblocked_read.error().kind, ErrorKind::kCancelled);
}

TEST(TestDesktopMixedCanFlashTransport, nonOwningCloseDoesNotDestroyCallerSerial)
{
    FakeBackend *fake = nullptr;
    auto serial = make_serial(fake);
    bool destroyed = false;
    fake->destroyed = &destroyed;

    DesktopMixedCanFlashTransport transport(serial.get());
    ASSERT_TRUE(transport.close().has_value());
    ASSERT_TRUE(!destroyed);
    const bool still_callable = serial->is_serial_port_open();
    Q_UNUSED(still_callable);
    serial.reset();
    ASSERT_TRUE(destroyed);
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
