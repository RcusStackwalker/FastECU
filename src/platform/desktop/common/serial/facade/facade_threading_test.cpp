#include <QScopeGuard>
#include <QPointer>
#include <QThread>
#include <gtest/gtest.h>
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <QCoreApplication>
#include <QSemaphore>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>

#include <gmock/gmock.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/transport/fastecu_can_transport.h"
#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"

using namespace std::chrono_literals;

TEST(TestFacadeThreading, constructDestroy_withoutUse_noThreadNoHang)
{
    QElapsedTimer t;
    t.start();
    {
        SerialPortActions serial{[]() -> SerialBackend *
                                 { return nullptr; }}; // never used: the I/O thread must not start
    }
    ASSERT_TRUE(t.elapsed() < 1000) << "unused facade must construct/destruct instantly";
}

TEST(TestFacadeThreading, getSet_marshalsToBackendThread)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });

    ASSERT_TRUE(serial.set_add_ssm_header(true)); // first call: starts the I/O thread
    ASSERT_TRUE(fake != nullptr);
    ASSERT_TRUE(fake->thread() != QThread::currentThread()) << "backend must live on the I/O thread, not the caller's";
    ASSERT_EQ(serial.get_add_ssm_header(), true);

    serial.set_serial_port_baudrate("10400");
    ASSERT_EQ(serial.get_serial_port_baudrate(), QString("10400"));
}

TEST(TestFacadeThreading, scriptedRead_returnsThroughFacade)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });

    serial.set_add_ssm_header(false); // force backend creation
    const QByteArray expected("\x80\xf0\x10\x02\xaa\xbb\x11", 7);
    EXPECT_CALL(*fake, read_serial_data(100)).WillOnce(::testing::Return(expected));

    ASSERT_EQ(serial.read_serial_data(100), expected);
}

TEST(TestFacadeThreading, backendException_propagatesWithoutHangingAndCleansUp)
{
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("FASTECU_THROWING_BACKEND_CHILD", "1");
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(), {});
    ASSERT_TRUE(child.waitForStarted(1000)) << qPrintable(child.errorString());

    if (!child.waitForFinished(2000))
    {
        child.kill();
        child.waitForFinished(1000);
        FAIL() << "backend exception left the facade caller blocked";
    }

    ASSERT_EQ(child.exitStatus(), QProcess::NormalExit);
    ASSERT_EQ(child.exitCode(), 0);
}

TEST(TestFacadeThreading, transportAdapters_isOpenContainsBackendException)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    EXPECT_CALL(*fake, is_serial_port_open())
        .WillRepeatedly(::testing::Throw(std::runtime_error("scripted backend open-state failure")));

    bool open = true;
    try
    {
        open = ssm.isOpen();
    }
    catch (...)
    {
        FAIL() << "SSM standalone isOpen propagated a backend exception";
    }
    ASSERT_TRUE(!open);

    try
    {
        open = kline.isOpen();
    }
    catch (...)
    {
        FAIL() << "K-Line standalone isOpen propagated a backend exception";
    }
    ASSERT_TRUE(!open);

    try
    {
        open = can.isOpen();
    }
    catch (...)
    {
        FAIL() << "CAN standalone isOpen propagated a backend exception";
    }
    ASSERT_TRUE(!open);
}

TEST(TestFacadeThreading, transportAdapters_normalEmptyReadIsSuccess)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    fastecu::FakeCancellationToken cancellation;

    const auto ssm_result = ssm.read(10ms, cancellation);
    ASSERT_TRUE(ssm_result.has_value());
    ASSERT_TRUE(!ssm_result->has_value());

    const auto kline_result = kline.read(10ms, cancellation);
    ASSERT_TRUE(kline_result.has_value());
    ASSERT_TRUE(!kline_result->has_value());

    const auto can_result = can.read(10ms, cancellation);
    ASSERT_TRUE(can_result.has_value());
    ASSERT_TRUE(!can_result->has_value());
}

TEST(TestFacadeThreading, transportAdapters_preCancelledReadSkipsBackend)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    fastecu::FakeCancellationToken cancellation;
    cancellation.set_cancelled(true);

    const auto ssm_result = ssm.read(10ms, cancellation);
    ASSERT_TRUE(!ssm_result.has_value());
    ASSERT_TRUE(ssm_result.error().kind == fastecu::ErrorKind::kCancelled);

    const auto kline_result = kline.read(10ms, cancellation);
    ASSERT_TRUE(!kline_result.has_value());
    ASSERT_TRUE(kline_result.error().kind == fastecu::ErrorKind::kCancelled);

    const auto can_result = can.read(10ms, cancellation);
    ASSERT_TRUE(!can_result.has_value());
    ASSERT_TRUE(can_result.error().kind == fastecu::ErrorKind::kCancelled);
}

TEST(TestFacadeThreading, transportAdapters_postCallCancellationPrecedesDisconnect)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    fastecu::FakeCancellationToken cancellation;
    std::atomic<bool> port_open{true};
    ON_CALL(*fake, is_serial_port_open()).WillByDefault([&port_open] { return port_open.load(); });
    EXPECT_CALL(*fake, read_serial_data(::testing::_))
        .WillRepeatedly(
            [&cancellation, &port_open](std::uint16_t) -> QByteArray
            {
                cancellation.set_cancelled(true);
                port_open.store(false);
                return QByteArray{};
            });

    const auto ssm_result = ssm.read(10ms, cancellation);
    ASSERT_TRUE(!ssm_result.has_value());
    ASSERT_TRUE(ssm_result.error().kind == fastecu::ErrorKind::kCancelled);

    cancellation.set_cancelled(false);
    port_open.store(true);
    const auto kline_result = kline.read(10ms, cancellation);
    ASSERT_TRUE(!kline_result.has_value());
    ASSERT_TRUE(kline_result.error().kind == fastecu::ErrorKind::kCancelled);

    cancellation.set_cancelled(false);
    port_open.store(true);
    const auto can_result = can.read(10ms, cancellation);
    ASSERT_TRUE(!can_result.has_value());
    ASSERT_TRUE(can_result.error().kind == fastecu::ErrorKind::kCancelled);
}

TEST(TestFacadeThreading, transportAdapters_backendReadExceptionMapsToInternal)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    fastecu::FakeCancellationToken cancellation;
    EXPECT_CALL(*fake, read_serial_data(::testing::_))
        .WillRepeatedly(::testing::Throw(std::runtime_error("scripted backend read failure")));

    const auto ssm_result = ssm.read(10ms, cancellation);
    ASSERT_TRUE(!ssm_result.has_value());
    ASSERT_TRUE(ssm_result.error().kind == fastecu::ErrorKind::kInternal);

    const auto kline_result = kline.read(10ms, cancellation);
    ASSERT_TRUE(!kline_result.has_value());
    ASSERT_TRUE(kline_result.error().kind == fastecu::ErrorKind::kInternal);

    const auto can_result = can.read(10ms, cancellation);
    ASSERT_TRUE(!can_result.has_value());
    ASSERT_TRUE(can_result.error().kind == fastecu::ErrorKind::kInternal);
}

TEST(TestFacadeThreading, canTransport_truncatedFrameMapsToInternal)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).WillOnce(::testing::Return(QByteArray("\x01\x02\x03", 3)));
    cdbg::FastEcuCanTransport can(&serial);
    fastecu::FakeCancellationToken cancellation;

    const auto result = can.read(10ms, cancellation);
    ASSERT_TRUE(!result.has_value());
    ASSERT_TRUE(result.error().kind == fastecu::ErrorKind::kInternal);
}

TEST(TestFacadeThreading, transportAdapters_nullOrClosedAdapterReturnsDisconnectedBeforeOperation)
{
    // A null serial pointer: adapters must fail without ever touching a backend.
    {
        FastEcuSsmTransport ssm(nullptr);
        mutdma::FastEcuKlineTransport kline(nullptr);
        cdbg::FastEcuCanTransport can(nullptr);
        fastecu::FakeCancellationToken cancellation;

        ASSERT_TRUE(!ssm.isOpen());
        ASSERT_TRUE(!kline.isOpen());
        ASSERT_TRUE(!can.isOpen());

        const auto ssm_write = ssm.write(bytes::ByteView());
        ASSERT_TRUE(!ssm_write.has_value());
        ASSERT_TRUE(ssm_write.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto kline_write = kline.write(bytes::ByteView());
        ASSERT_TRUE(!kline_write.has_value());
        ASSERT_TRUE(kline_write.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto kline_baud = kline.setBaud(10400);
        ASSERT_TRUE(!kline_baud.has_value());
        ASSERT_TRUE(kline_baud.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto can_write = can.write(0x123, bytes::ByteView());
        ASSERT_TRUE(!can_write.has_value());
        ASSERT_TRUE(can_write.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto ssm_read = ssm.read(10ms, cancellation);
        ASSERT_TRUE(!ssm_read.has_value());
        ASSERT_TRUE(ssm_read.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto kline_read = kline.read(10ms, cancellation);
        ASSERT_TRUE(!kline_read.has_value());
        ASSERT_TRUE(kline_read.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto can_read = can.read(10ms, cancellation);
        ASSERT_TRUE(!can_read.has_value());
        ASSERT_TRUE(can_read.error().kind == fastecu::ErrorKind::kDisconnected);
    }

    // A closed (but non-null) adapter: same Disconnected mapping, reached
    // through the backend's is_serial_port_open() rather than a null check.
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial(
            [&fake]() -> SerialBackend *
            {
                fake = new NiceFakeBackend();
                return fake;
            });
        serial.set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillRepeatedly(::testing::Return(false));
        FastEcuSsmTransport ssm(&serial);
        mutdma::FastEcuKlineTransport kline(&serial);
        cdbg::FastEcuCanTransport can(&serial);
        fastecu::FakeCancellationToken cancellation;

        const auto ssm_write = ssm.write(bytes::ByteView());
        ASSERT_TRUE(!ssm_write.has_value());
        ASSERT_TRUE(ssm_write.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto kline_write = kline.write(bytes::ByteView());
        ASSERT_TRUE(!kline_write.has_value());
        ASSERT_TRUE(kline_write.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto kline_baud = kline.setBaud(10400);
        ASSERT_TRUE(!kline_baud.has_value());
        ASSERT_TRUE(kline_baud.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto can_write = can.write(0x123, bytes::ByteView());
        ASSERT_TRUE(!can_write.has_value());
        ASSERT_TRUE(can_write.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto ssm_read = ssm.read(10ms, cancellation);
        ASSERT_TRUE(!ssm_read.has_value());
        ASSERT_TRUE(ssm_read.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto kline_read = kline.read(10ms, cancellation);
        ASSERT_TRUE(!kline_read.has_value());
        ASSERT_TRUE(kline_read.error().kind == fastecu::ErrorKind::kDisconnected);

        const auto can_read = can.read(10ms, cancellation);
        ASSERT_TRUE(!can_read.has_value());
        ASSERT_TRUE(can_read.error().kind == fastecu::ErrorKind::kDisconnected);
    }
}

TEST(TestFacadeThreading, transportAdapters_writeSuccessAndCanFrameEncoding)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);

    const bytes::Bytes ssm_payload{0x11, 0x22, 0x33};
    EXPECT_CALL(*fake, write_serial_data_echo_check(QByteArray::fromHex("112233")))
        .WillOnce(::testing::Return(QByteArray{}));
    const auto ssm_result = ssm.write(bytes::ByteView(ssm_payload));
    ASSERT_TRUE(ssm_result.has_value());
    ASSERT_EQ(*ssm_result, ssm_payload.size());

    const bytes::Bytes kline_payload{0xAA, 0xBB};
    EXPECT_CALL(*fake, write_serial_data(QByteArray::fromHex("aabb"))).WillOnce(::testing::Return(QByteArray{}));
    const auto kline_result = kline.write(bytes::ByteView(kline_payload));
    ASSERT_TRUE(kline_result.has_value());
    ASSERT_EQ(*kline_result, kline_payload.size());

    // CAN wire convention: 4 big-endian CAN-id bytes followed by the payload
    // verbatim -- confirm the adapter builds exactly that frame, byte for byte.
    const bytes::Bytes can_payload{0xDE, 0xAD, 0xBE, 0xEF};
    QByteArray expected_frame;
    bytes::appendU32Be(expected_frame, 0x123);
    expected_frame.append(bytes::toQByteArray(bytes::ByteView(can_payload)));
    EXPECT_CALL(*fake, write_serial_data_echo_check(expected_frame)).WillOnce(::testing::Return(QByteArray{}));
    const auto can_result = can.write(0x123, bytes::ByteView(can_payload));
    ASSERT_TRUE(can_result.has_value());
    ASSERT_EQ(*can_result, can_payload.size());
}

TEST(TestFacadeThreading, transportAdapters_disconnectDuringWriteMapsToDisconnected)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);

    {
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data(::testing::_)).WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
    }

    const auto ssm_result = ssm.write(bytes::ByteView());
    ASSERT_TRUE(!ssm_result.has_value());
    ASSERT_TRUE(ssm_result.error().kind == fastecu::ErrorKind::kDisconnected);

    const auto kline_result = kline.write(bytes::ByteView());
    ASSERT_TRUE(!kline_result.has_value());
    ASSERT_TRUE(kline_result.error().kind == fastecu::ErrorKind::kDisconnected);

    const auto can_result = can.write(0x123, bytes::ByteView());
    ASSERT_TRUE(!can_result.has_value());
    ASSERT_TRUE(can_result.error().kind == fastecu::ErrorKind::kDisconnected);
}

TEST(TestFacadeThreading, transportAdapters_disconnectDuringReadMapsToDisconnected)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    fastecu::FakeCancellationToken cancellation; // never cancelled: isolates the
                                                 // disconnect check from the
                                                 // cancellation-precedence path

    {
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(10)).WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(10)).WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(10)).WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
    }

    const auto ssm_result = ssm.read(10ms, cancellation);
    ASSERT_TRUE(!ssm_result.has_value());
    ASSERT_TRUE(ssm_result.error().kind == fastecu::ErrorKind::kDisconnected);

    const auto kline_result = kline.read(10ms, cancellation);
    ASSERT_TRUE(!kline_result.has_value());
    ASSERT_TRUE(kline_result.error().kind == fastecu::ErrorKind::kDisconnected);

    const auto can_result = can.read(10ms, cancellation);
    ASSERT_TRUE(!can_result.has_value());
    ASSERT_TRUE(can_result.error().kind == fastecu::ErrorKind::kDisconnected);
}

TEST(TestFacadeThreading, transportAdapters_backendWriteExceptionMapsToInternal)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);

    {
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend write failure")));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data(::testing::_))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend write failure")));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend write failure")));
    }

    const auto ssm_result = ssm.write(bytes::ByteView());
    ASSERT_TRUE(!ssm_result.has_value());
    ASSERT_TRUE(ssm_result.error().kind == fastecu::ErrorKind::kInternal);

    const auto kline_result = kline.write(bytes::ByteView());
    ASSERT_TRUE(!kline_result.has_value());
    ASSERT_TRUE(kline_result.error().kind == fastecu::ErrorKind::kInternal);

    const auto can_result = can.write(0x123, bytes::ByteView());
    ASSERT_TRUE(!can_result.has_value());
    ASSERT_TRUE(can_result.error().kind == fastecu::ErrorKind::kInternal);
}

TEST(TestFacadeThreading, transportAdapters_backendNonStandardExceptionMapsToInternal)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    fastecu::FakeCancellationToken cancellation;

    // A throw that is not a std::exception must still land in Internal via
    // the adapters' generic `catch (...)` branch, not escape or crash.
    {
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());
    }
    const auto ssm_read = ssm.read(10ms, cancellation);
    ASSERT_TRUE(!ssm_read.has_value());
    ASSERT_TRUE(ssm_read.error().kind == fastecu::ErrorKind::kInternal);

    const auto kline_read = kline.read(10ms, cancellation);
    ASSERT_TRUE(!kline_read.has_value());
    ASSERT_TRUE(kline_read.error().kind == fastecu::ErrorKind::kInternal);

    const auto can_read = can.read(10ms, cancellation);
    ASSERT_TRUE(!can_read.has_value());
    ASSERT_TRUE(can_read.error().kind == fastecu::ErrorKind::kInternal);

    const auto ssm_write = ssm.write(bytes::ByteView());
    ASSERT_TRUE(!ssm_write.has_value());
    ASSERT_TRUE(ssm_write.error().kind == fastecu::ErrorKind::kInternal);

    const auto kline_write = kline.write(bytes::ByteView());
    ASSERT_TRUE(!kline_write.has_value());
    ASSERT_TRUE(kline_write.error().kind == fastecu::ErrorKind::kInternal);

    const auto can_write = can.write(0x123, bytes::ByteView());
    ASSERT_TRUE(!can_write.has_value());
    ASSERT_TRUE(can_write.error().kind == fastecu::ErrorKind::kInternal);
}

TEST(TestFacadeThreading, transportAdapters_cancellationPrecedesReadException)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);
    fastecu::FakeCancellationToken cancellation;
    // Cancel right as the backend is about to throw: the adapter's catch
    // block must report Cancelled, not Internal, once cancellation was
    // observed -- even though the backend failed via exception, not silence.
    EXPECT_CALL(*fake, is_serial_port_open()).WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(*fake, read_serial_data(::testing::_))
        .WillRepeatedly(
            [&cancellation](std::uint16_t) -> QByteArray
            {
                cancellation.set_cancelled(true);
                throw std::runtime_error("scripted backend read failure");
            });

    const auto ssm_result = ssm.read(10ms, cancellation);
    ASSERT_TRUE(!ssm_result.has_value());
    ASSERT_TRUE(ssm_result.error().kind == fastecu::ErrorKind::kCancelled);

    cancellation.set_cancelled(false);
    const auto kline_result = kline.read(10ms, cancellation);
    ASSERT_TRUE(!kline_result.has_value());
    ASSERT_TRUE(kline_result.error().kind == fastecu::ErrorKind::kCancelled);

    cancellation.set_cancelled(false);
    const auto can_result = can.read(10ms, cancellation);
    ASSERT_TRUE(!can_result.has_value());
    ASSERT_TRUE(can_result.error().kind == fastecu::ErrorKind::kCancelled);
}

TEST(TestFacadeThreading, klineTransport_setBaudSuccessRejectionDisconnectException)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false);
    mutdma::FastEcuKlineTransport kline(&serial);

    ::testing::InSequence sequence;
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, change_port_speed(QString("10400"))).WillOnce(::testing::Return(kSerialSuccess));
    const auto success = kline.setBaud(10400);
    ASSERT_TRUE(success.has_value());

    // Rejection: driver returns non-zero but the port stays open.
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, change_port_speed(QString("10400"))).WillOnce(::testing::Return(kSerialError));
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    const auto rejected = kline.setBaud(10400);
    ASSERT_TRUE(!rejected.has_value());
    ASSERT_TRUE(rejected.error().kind == fastecu::ErrorKind::kInternal);

    // Disconnect: driver returns non-zero and the port is found closed
    // immediately afterward.
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, change_port_speed(QString("10400"))).WillOnce(::testing::Return(kSerialError));
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
    const auto disconnected = kline.setBaud(10400);
    ASSERT_TRUE(!disconnected.has_value());
    ASSERT_TRUE(disconnected.error().kind == fastecu::ErrorKind::kDisconnected);
    // Exception: driver throws instead of returning.
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, change_port_speed(QString("10400")))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend baud-change failure")));
    const auto thrown = kline.setBaud(10400);
    ASSERT_TRUE(!thrown.has_value());
    ASSERT_TRUE(thrown.error().kind == fastecu::ErrorKind::kInternal);

    // Non-standard exception: still mapped to Internal via catch(...).
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, change_port_speed(QString("10400"))).WillOnce(ThrowNonStandardBackendFailure());
    const auto thrown_non_standard = kline.setBaud(10400);
    ASSERT_TRUE(!thrown_non_standard.has_value());
    ASSERT_TRUE(thrown_non_standard.error().kind == fastecu::ErrorKind::kInternal);
}

// ---- affinity-warning capture ------------------------------------------
static QStringList g_thread_warnings;
static QtMessageHandler g_prev_handler = nullptr;

static void warningCapture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (type == QtWarningMsg && (msg.contains("another thread") || msg.contains("different thread")))
    {
        g_thread_warnings.append(msg);
    }
    if (g_prev_handler)
    {
        g_prev_handler(type, ctx, msg);
    }
}

TEST(TestFacadeThreading, workerThreadCaller_noAffinityWarnings)
{
    // The exact LoggingWorker scenario from the bench checklist: a non-GUI
    // thread drives the facade. Data must arrive and Qt must emit no
    // cross-thread affinity warnings.
    g_thread_warnings.clear();
    g_prev_handler = qInstallMessageHandler(warningCapture);

    const QByteArray expected("\x80\xf0\x10\x01\x55\x66", 6);
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake, &expected]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            EXPECT_CALL(*fake, read_serial_data(100)).WillOnce(::testing::Return(expected));
            return fake;
        });

    QByteArray got;
    std::thread worker(
        [&]
        {
            serial.set_add_ssm_header(true);
            got = serial.read_serial_data(100);
        });
    worker.join();

    qInstallMessageHandler(g_prev_handler);
    ASSERT_EQ(got, expected);
    ASSERT_TRUE(g_thread_warnings.isEmpty()) << qPrintable("affinity warnings: " + g_thread_warnings.join(" | "));
}

TEST(TestFacadeThreading, concurrentCallers_serializeWithoutInterleaving)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });
    serial.set_add_ssm_header(false); // create backend

    std::atomic<int> active_calls{0};
    std::atomic<bool> interleaved{false};
    auto serialized_call = [&active_calls, &interleaved](auto&&) -> QByteArray
    {
        if (active_calls.fetch_add(1) != 0)
        {
            interleaved.store(true);
        }
        QThread::msleep(20);
        active_calls.fetch_sub(1);
        return {};
    };
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(10).WillRepeatedly(serialized_call);
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(10).WillRepeatedly(serialized_call);

    auto hammer = [&serial](int n)
    {
        for (int i = 0; i < n; ++i)
        {
            serial.write_serial_data(QByteArray(1, char(i)));
            serial.read_serial_data(10);
        }
    };
    std::thread a(hammer, 5), b(hammer, 5);
    a.join();
    b.join();

    ASSERT_TRUE(!interleaved.load());
    ASSERT_EQ(active_calls.load(), 0);
}

TEST(TestFacadeThreading, destroyAfterUse_joinsIoThread)
{
    QPointer<QThread> io_thread;
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial(
            [&fake]() -> SerialBackend *
            {
                fake = new NiceFakeBackend();
                return fake;
            });
        serial.set_add_ssm_header(true);
        io_thread = fake->thread();
        ASSERT_TRUE(io_thread && io_thread->isRunning());
    }
    ASSERT_TRUE(!io_thread || !io_thread->isRunning()) << "facade destruction must stop and join the I/O thread";
}

TEST(TestFacadeThreading, destroyWhileReadInFlight_waitsForBackendCall)
{
    FakeBackend *fake = nullptr;
    auto *serial = new SerialPortActions(
        [&fake]() -> SerialBackend *
        {
            fake = new NiceFakeBackend();
            return fake;
        });

    serial->set_add_ssm_header(false); // create backend before wiring gates

    QSemaphore read_entered;
    QSemaphore continue_read;
    EXPECT_CALL(*fake, read_serial_data(10))
        .WillOnce(
            [&read_entered, &continue_read](std::uint16_t)
            {
                read_entered.release();
                continue_read.acquire();
                return QByteArray("done");
            });

    QByteArray got;
    std::thread reader([&] { got = serial->read_serial_data(10); });
    std::thread destroyer;
    bool deletion_scheduled = false;
    const auto cleanup = qScopeGuard(
        [&]
        {
            continue_read.release();
            if (reader.joinable())
            {
                reader.join();
            }
            if (destroyer.joinable())
            {
                destroyer.join();
            }
            if (!deletion_scheduled)
            {
                delete serial;
            }
        });
    ASSERT_TRUE(read_entered.tryAcquire(1, 1000)) << "backend read did not start";

    std::atomic<bool> destroyed{false};
    destroyer = std::thread(
        [&]
        {
            delete serial;
            destroyed.store(true);
        });
    deletion_scheduled = true;

    fastecu::testing::process_events_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(!destroyed.load()) << "facade teardown must wait for the in-flight backend call";

    continue_read.release();
    reader.join();
    destroyer.join();

    ASSERT_EQ(got, QByteArray("done"));
    ASSERT_TRUE(destroyed.load());
}

int run_throwing_backend_child()
{
    bool backend_destroyed = false;
    bool exception_propagated = false;
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial(
            [&fake]() -> SerialBackend *
            {
                fake = new NiceFakeBackend();
                return fake;
            });
        serial.set_add_ssm_header(false);
        EXPECT_CALL(*fake, read_serial_data(10))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend read failure")));
        fake->destroyed = &backend_destroyed;

        try
        {
            serial.read_serial_data(10);
        }
        catch (const std::runtime_error& error)
        {
            exception_propagated = QString::fromUtf8(error.what()) == QStringLiteral("scripted backend read failure");
        }
    }
    return exception_propagated && backend_destroyed && !::testing::Test::HasFailure() ? 0 : 1;
}
