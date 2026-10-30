#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/backend/ports/testing/result_matchers.h"
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
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::_;
using ::testing::Eq;
using ::testing::Optional;

constexpr MixedCanConfig Config()
{
    return MixedCanConfig{
        .kernel = Iso15765Config{.bitrate = 500000, .request_id = 2016, .response_id = 2024, .extended_id = false},
        .bootloader = RawCanConfig{.bitrate = 500000, .transmit_id = 1048574, .receive_id = 33, .extended_id = true},
    };
}

std::unique_ptr<SerialPortActions> MakeSerial(FakeBackend *& fake)
{
    auto serial = std::make_unique<SerialPortActions>(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial->SetAddSsmHeader(false);
    EXPECT_CALL(*fake, OpenSerialPort()).WillRepeatedly(::testing::Return(QStringLiteral("fake-adapter")));
    return serial;
}

void ConfigureAndOpen(DesktopMixedCanFlashTransport& transport)
{
    ASSERT_THAT(transport.Configure(Config()), IsOk());
    ASSERT_THAT(transport.Open(), IsOk());
}

} // namespace

TEST(TestDesktopMixedCanFlashTransport, initialResetReachesBackendAndReturnsFailure)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    EXPECT_CALL(*fake, ResetConnection()).WillOnce(::testing::Return());

    ASSERT_THAT(transport.ResetConnection(), IsOk());

    EXPECT_CALL(*fake, ResetConnection())
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend reset failure")));
    const auto failed = transport.ResetConnection();
    EXPECT_THAT(failed, IsErr(ErrorKind::kInternal));
}

TEST(TestDesktopMixedCanFlashTransport, initialResetAfterCloseReturnsDisconnected)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    ASSERT_THAT(transport.Close(), IsOk());

    const auto result = transport.ResetConnection();

    EXPECT_THAT(result, IsErr(ErrorKind::kDisconnected));
}

TEST(TestDesktopMixedCanFlashTransport, configuresIsoThenTransitionsRawAndBack)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));

    ::testing::InSequence sequence;
    EXPECT_CALL(*fake, SetIsIso14230Connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIsCanConnection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIsIso15765Connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIs29BitId(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetCanSpeed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetCanSourceAddress(1048574)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetCanDestinationAddress(33)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIso15765SourceAddress(2016)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIso15765DestinationAddress(2024)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetAddIso14230Header(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, OpenSerialPort()).WillOnce(::testing::Return(QStringLiteral("fake-adapter")));
    EXPECT_CALL(*fake, ResetConnection()).WillOnce(::testing::Return());
    EXPECT_CALL(*fake, SetIsIso14230Connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIsCanConnection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIsIso15765Connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIs29BitId(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetCanSpeed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetCanSourceAddress(1048574)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetCanDestinationAddress(33)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetAddIso14230Header(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, OpenSerialPort()).WillOnce(::testing::Return(QStringLiteral("fake-adapter")));
    EXPECT_CALL(*fake, ResetConnection()).WillOnce(::testing::Return());
    EXPECT_CALL(*fake, SetIsIso14230Connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIsCanConnection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIsIso15765Connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIs29BitId(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetCanSpeed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetCanSourceAddress(1048574)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetCanDestinationAddress(33)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIso15765SourceAddress(2016)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetIso15765DestinationAddress(2024)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, SetAddIso14230Header(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, OpenSerialPort()).WillOnce(::testing::Return(QStringLiteral("fake-adapter")));

    ASSERT_THAT(transport.Configure(Config()), IsOk());
    ASSERT_THAT(transport.Open(), IsOk());
    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());
    ASSERT_THAT(transport.EnterIso15765KernelMode(), IsOk());
}

TEST(TestDesktopMixedCanFlashTransport, everyModeConfigurationClearsStickyIso14230HeaderState)
{
    FakeBackend *fake = nullptr;
    auto serial = MakeSerial(fake);
    SerialPortActions *observed = serial.get();
    ASSERT_TRUE(observed->SetAddIso14230Header(true));
    DesktopMixedCanFlashTransport transport(std::move(serial));

    ASSERT_THAT(transport.Configure(Config()), IsOk());
    ASSERT_EQ(observed->GetAddIso14230Header(), false);
    ASSERT_THAT(transport.Open(), IsOk());

    ASSERT_TRUE(observed->SetAddIso14230Header(true));
    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());
    ASSERT_EQ(observed->GetAddIso14230Header(), false);

    ASSERT_TRUE(observed->SetAddIso14230Header(true));
    ASSERT_THAT(transport.EnterIso15765KernelMode(), IsOk());
    ASSERT_EQ(observed->GetAddIso14230Header(), false);
}

TEST(TestDesktopMixedCanFlashTransport, preservesExtendedIsoIdDuringInitialConfigurationAndReturnTransition)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    MixedCanConfig extended = Config();
    extended.kernel.extended_id = true;
    EXPECT_CALL(*fake, SetIs29BitId(true)).Times(3);

    ASSERT_THAT(transport.Configure(extended), IsOk());
    ASSERT_THAT(transport.Open(), IsOk());
    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());
    ASSERT_THAT(transport.EnterIso15765KernelMode(), IsOk());
}

TEST(TestDesktopMixedCanFlashTransport, rawFrameAddsAndParsesBigEndianId)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());

    const QByteArray expected_write = QByteArray::fromHex("000ffffe7a90000000000000");
    EXPECT_CALL(*fake, WriteSerialDataEchoCheck(expected_write)).WillOnce(::testing::Return(QByteArray{}));
    ASSERT_THAT(transport.WriteRaw({0x000ffffe, {0x7a, 0x90, 0, 0, 0, 0, 0, 0}}, cancellation), IsOk());

    EXPECT_CALL(*fake, ReadSerialData(::testing::_))
        .WillOnce(::testing::Return(QByteArray::fromHex("000000217a96000000000000")));
    const auto frame = transport.ReadRaw(800ms, cancellation);
    ASSERT_THAT(frame, IsOk());
    ASSERT_THAT(*frame, Optional(_));
    ASSERT_EQ(frame->value().id, 0x21U);
    ASSERT_EQ(frame->value().payload, (bytes::Bytes{0x7a, 0x96, 0, 0, 0, 0, 0, 0}));
}

TEST(TestDesktopMixedCanFlashTransport, configureFailsAtEverySetter)
{
    const std::array<std::function<void(FakeBackend&)>, 10> failures{
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetIsIso14230Connection(false)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetIsCanConnection(false)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetIsIso15765Connection(true)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetIs29BitId(false)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetCanSpeed(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, SetCanSourceAddress(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, SetCanDestinationAddress(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, SetIso15765SourceAddress(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, SetIso15765DestinationAddress(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetAddIso14230Header(false)).WillOnce(::testing::Return(false)); },
    };
    for (const auto& set_failure : failures)
    {
        SCOPED_TRACE(::testing::PrintToString(set_failure));
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(MakeSerial(fake));
        set_failure(*fake);
        const auto result = transport.Configure(Config());
        EXPECT_THAT(result, IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(TestDesktopMixedCanFlashTransport, configureRejectsReconfigureWhileAlreadyConfigured)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));

    const auto reconfigure = transport.Configure(Config());

    EXPECT_THAT(reconfigure, IsErr(ErrorKind::kInvalidConfig));
}

TEST(TestDesktopMixedCanFlashTransport, poisonedTransitionMakesConfigureAndOpenSurfaceTheStaleErrorEvenAfterClose)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
    EXPECT_CALL(*fake, OpenSerialPort()).WillOnce(::testing::Return(QString{}));

    const auto transition = transport.EnterRawBootloaderMode();
    EXPECT_THAT(transition, IsErr(ErrorKind::kDisconnected));

    // close() must not clear the poison: it only tears down the live handle.
    ASSERT_THAT(transport.Close(), IsOk());

    const auto reconfigure = transport.Configure(Config());
    EXPECT_THAT(reconfigure, IsErr(ErrorKind::kDisconnected));

    const auto reopen = transport.Open();
    EXPECT_THAT(reopen, IsErr(ErrorKind::kDisconnected));
}

TEST(TestDesktopMixedCanFlashTransport, rawTransitionFailsAtEverySetterAndMakesIoTerminal)
{
    const std::array<std::function<void(FakeBackend&)>, 8> failures{
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetIsIso14230Connection(false)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetIsCanConnection(true)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetIsIso15765Connection(false)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetIs29BitId(true)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetCanSpeed(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, SetCanSourceAddress(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, SetCanDestinationAddress(::testing::_)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetAddIso14230Header(false)).WillOnce(::testing::Return(false)); },
    };
    for (const auto& set_failure : failures)
    {
        SCOPED_TRACE(::testing::PrintToString(set_failure));
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(MakeSerial(fake));
        FakeCancellationToken cancellation;
        ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
        set_failure(*fake);

        const auto transition = transport.EnterRawBootloaderMode();
        EXPECT_THAT(transition, IsErr(ErrorKind::kInvalidConfig));
        EXPECT_CALL(*fake, WriteSerialDataEchoCheck(::testing::_)).Times(0);
        const auto write = transport.WriteIso15765(bytes::Bytes{0x01}, cancellation);
        EXPECT_THAT(write, IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(TestDesktopMixedCanFlashTransport, failedReopenMakesFollowingIoTerminal)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
    EXPECT_CALL(*fake, OpenSerialPort()).WillOnce(::testing::Return(QString{}));

    const auto transition = transport.EnterRawBootloaderMode();
    EXPECT_THAT(transition, IsErr(ErrorKind::kDisconnected));
    EXPECT_CALL(*fake, WriteSerialDataEchoCheck(::testing::_)).Times(0);
    const auto write = transport.WriteRaw({0x000ffffe, {0x7a}}, cancellation);
    EXPECT_THAT(write, IsErr(ErrorKind::kDisconnected));
}

TEST(TestDesktopMixedCanFlashTransport, rawReadRejectsShortFrameAndWrongReceiveId)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());

    EXPECT_CALL(*fake, ReadSerialData(::testing::_)).WillOnce(::testing::Return(QByteArray::fromHex("000021")));
    const auto short_frame = transport.ReadRaw(10ms, cancellation);
    EXPECT_THAT(short_frame, IsErr(ErrorKind::kBadResponse));

    EXPECT_CALL(*fake, ReadSerialData(::testing::_)).WillOnce(::testing::Return(QByteArray::fromHex("000000227a96")));
    const auto wrong_id = transport.ReadRaw(10ms, cancellation);
    EXPECT_THAT(wrong_id, IsErr(ErrorKind::kBadResponse));
}

TEST(TestDesktopMixedCanFlashTransport, clearReceiveBufferRejectsBackendFailure)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());
    EXPECT_CALL(*fake, ClearRxBuffer()).WillOnce(::testing::Return(kSerialError));

    const auto result = transport.ClearReceiveBuffer();
    EXPECT_THAT(result, IsErr(ErrorKind::kInternal));
}

TEST(TestDesktopMixedCanFlashTransport, detectsDisconnectionBeforeAndDuringIo)
{
    ::testing::InSequence sequence;
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));

    EXPECT_CALL(*fake, IsSerialPortOpen()).WillOnce(::testing::Return(false));
    const auto before_write = transport.WriteIso15765(bytes::Bytes{0x01}, cancellation);
    EXPECT_THAT(before_write, IsErr(ErrorKind::kDisconnected));

    EXPECT_CALL(*fake, IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, ReadSerialData(::testing::_)).WillOnce(::testing::Return(QByteArray("\x01", 1)));
    EXPECT_CALL(*fake, IsSerialPortOpen()).WillOnce(::testing::Return(false));
    const auto during_read = transport.ReadIso15765(10ms, cancellation);
    EXPECT_THAT(during_read, IsErr(ErrorKind::kDisconnected));
}

TEST(TestDesktopMixedCanFlashTransport, catchesStandardAndNonstandardBackendExceptions)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));

    EXPECT_CALL(*fake, WriteSerialDataEchoCheck(::testing::_))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend write failure")));
    const auto standard = transport.WriteIso15765(bytes::Bytes{0x01}, cancellation);
    EXPECT_THAT(standard, IsErr(ErrorKind::kInternal));

    EXPECT_CALL(*fake, WriteSerialDataEchoCheck(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());
    const auto nonstandard = transport.WriteIso15765(bytes::Bytes{0x01}, cancellation);
    EXPECT_THAT(nonstandard, IsErr(ErrorKind::kInternal));
}

TEST(TestDesktopMixedCanFlashTransport, cancellationAndUnblockSuppressSubsequentIo)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    FakeCancellationToken cancelled(true);
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
    EXPECT_CALL(*fake, WriteSerialDataEchoCheck(::testing::_)).Times(0);

    const auto cancelled_write = transport.WriteIso15765(bytes::Bytes{0x01}, cancelled);
    EXPECT_THAT(cancelled_write, IsErr(ErrorKind::kCancelled));

    FakeCancellationToken cancellation;
    transport.RequestUnblock();
    EXPECT_CALL(*fake, ReadSerialData(::testing::_)).Times(0);
    const auto unblocked_read = transport.ReadIso15765(10ms, cancellation);
    EXPECT_THAT(unblocked_read, IsErr(ErrorKind::kCancelled));
}

TEST(TestDesktopMixedCanFlashTransport, rejectsIoAndTransitionsInTheWrongMode)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
    EXPECT_CALL(*fake, WriteSerialDataEchoCheck(_)).Times(0);
    EXPECT_CALL(*fake, ReadSerialData(_)).Times(0);
    EXPECT_CALL(*fake, ClearRxBuffer()).Times(0);
    const auto wrong_mode = [](std::string operation)
    { return IsErrWith(ErrorKind::kInvalidConfig, Eq("mixed CAN " + operation + " in wrong mode")); };

    EXPECT_THAT(transport.EnterIso15765KernelMode(), wrong_mode("ISO-15765 kernel transition"));
    EXPECT_THAT(transport.ClearReceiveBuffer(), wrong_mode("receive-buffer clear"));
    EXPECT_THAT(transport.WriteRaw({0x000ffffe, {0x7a}}, cancellation), wrong_mode("raw CAN write"));
    EXPECT_THAT(transport.ReadRaw(10ms, cancellation), wrong_mode("raw CAN read"));

    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());
    EXPECT_THAT(transport.EnterRawBootloaderMode(), wrong_mode("raw bootloader transition"));
    EXPECT_THAT(transport.WriteIso15765(bytes::Bytes{0x01}, cancellation), wrong_mode("ISO-15765 write"));
    EXPECT_THAT(transport.ReadIso15765(10ms, cancellation), wrong_mode("ISO-15765 read"));
}

TEST(TestDesktopMixedCanFlashTransport, rawTransitionResetFailureMakesIoTerminal)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
    EXPECT_CALL(*fake, ResetConnection()).WillOnce(::testing::Throw(std::runtime_error("scripted reset failure")));
    EXPECT_CALL(*fake, SetIsCanConnection(true)).Times(0);

    EXPECT_THAT(transport.EnterRawBootloaderMode(), IsErr(ErrorKind::kInternal));
    EXPECT_THAT(transport.WriteRaw({0x000ffffe, {0x7a}}, cancellation), IsErr(ErrorKind::kInternal));
}

TEST(TestDesktopMixedCanFlashTransport, kernelTransitionFailuresMakeIoTerminal)
{
    const std::array<std::function<void(FakeBackend&)>, 3> failures{
        [](FakeBackend& fake)
        { EXPECT_CALL(fake, ResetConnection()).WillOnce(::testing::Throw(std::runtime_error("scripted reset"))); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, SetIsIso15765Connection(true)).WillOnce(::testing::Return(false)); },
        [](FakeBackend& fake) { EXPECT_CALL(fake, OpenSerialPort()).WillOnce(::testing::Return(QString{})); },
    };
    const std::array<ErrorKind, 3> kinds{ErrorKind::kInternal, ErrorKind::kInvalidConfig, ErrorKind::kDisconnected};
    for (std::size_t index = 0; index < failures.size(); ++index)
    {
        SCOPED_TRACE(index);
        FakeBackend *fake = nullptr;
        DesktopMixedCanFlashTransport transport(MakeSerial(fake));
        FakeCancellationToken cancellation;
        ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
        ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());
        failures.at(index)(*fake);

        EXPECT_THAT(transport.EnterIso15765KernelMode(), IsErr(kinds.at(index)));
        EXPECT_CALL(*fake, WriteSerialDataEchoCheck(_)).Times(0);
        EXPECT_THAT(transport.WriteIso15765(bytes::Bytes{0x01}, cancellation), IsErr(kinds.at(index)));
    }
}

TEST(TestDesktopMixedCanFlashTransport, rawIoRejectsOversizedPayloadsAndPassesEmptyAndCancelledReads)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    FakeCancellationToken cancellation;
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());

    EXPECT_CALL(*fake, WriteSerialDataEchoCheck(_)).Times(0);
    EXPECT_THAT(transport.WriteRaw({0x000ffffe, bytes::Bytes(9, 0x00)}, cancellation),
                IsErrWith(ErrorKind::kInvalidConfig, Eq("raw CAN payload exceeds 8 bytes")));

    EXPECT_CALL(*fake, ReadSerialData(_)).WillOnce(::testing::Return(QByteArray{}));
    EXPECT_THAT(transport.ReadRaw(10ms, cancellation), fastecu::testing::IsOkAnd(Eq(std::nullopt)));

    EXPECT_CALL(*fake, ReadSerialData(_))
        .WillOnce(::testing::Return(QByteArray::fromHex("00000021000000000000000000")));
    EXPECT_THAT(transport.ReadRaw(10ms, cancellation),
                IsErrWith(ErrorKind::kBadResponse, Eq("raw CAN response payload exceeds 8 bytes")));

    FakeCancellationToken cancelled(true);
    EXPECT_THAT(transport.ReadRaw(10ms, cancelled), IsErr(ErrorKind::kCancelled));
}

TEST(TestDesktopMixedCanFlashTransport, clearReceiveBufferRejectsAClosedPort)
{
    FakeBackend *fake = nullptr;
    DesktopMixedCanFlashTransport transport(MakeSerial(fake));
    ASSERT_NO_FATAL_FAILURE(ConfigureAndOpen(transport));
    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());
    EXPECT_CALL(*fake, IsSerialPortOpen()).WillOnce(::testing::Return(false));
    EXPECT_CALL(*fake, ClearRxBuffer()).Times(0);

    EXPECT_THAT(transport.ClearReceiveBuffer(),
                IsErrWith(ErrorKind::kDisconnected, Eq("mixed CAN adapter disconnected before receive-buffer clear")));
}

TEST(TestDesktopMixedCanFlashTransport, nonOwningCloseDoesNotDestroyCallerSerial)
{
    FakeBackend *fake = nullptr;
    auto serial = MakeSerial(fake);
    bool destroyed = false;
    fake->destroyed = &destroyed;

    DesktopMixedCanFlashTransport transport(serial.get());
    ASSERT_THAT(transport.Close(), IsOk());
    ASSERT_TRUE(!destroyed);
    const bool still_callable = serial->IsSerialPortOpen();
    Q_UNUSED(still_callable);
    serial.reset();
    ASSERT_TRUE(destroyed);
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
