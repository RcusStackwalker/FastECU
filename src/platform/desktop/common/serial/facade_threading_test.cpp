#include <QtTest>
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
#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/transport/fastecu_can_transport.h"
#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"

using namespace std::chrono_literals;

class TestFacadeThreading : public QObject
{
    Q_OBJECT
  private slots:
    void constructDestroy_withoutUse_noThreadNoHang();
    void getSet_marshalsToBackendThread();
    void scriptedRead_returnsThroughFacade();
    void backendException_propagatesWithoutHangingAndCleansUp();
    void transportAdapters_isOpenContainsBackendException();
    void transportAdapters_normalEmptyReadIsSuccess();
    void transportAdapters_preCancelledReadSkipsBackend();
    void transportAdapters_postCallCancellationPrecedesDisconnect();
    void transportAdapters_backendReadExceptionMapsToInternal();
    void canTransport_truncatedFrameMapsToInternal();
    void transportAdapters_nullOrClosedAdapterReturnsDisconnectedBeforeOperation();
    void transportAdapters_writeSuccessAndCanFrameEncoding();
    void transportAdapters_disconnectDuringWriteMapsToDisconnected();
    void transportAdapters_disconnectDuringReadMapsToDisconnected();
    void transportAdapters_backendWriteExceptionMapsToInternal();
    void transportAdapters_backendNonStandardExceptionMapsToInternal();
    void transportAdapters_cancellationPrecedesReadException();
    void klineTransport_setBaudSuccessRejectionDisconnectException();
    void workerThreadCaller_noAffinityWarnings();
    void concurrentCallers_serializeWithoutInterleaving();
    void destroyAfterUse_joinsIoThread();
    void destroyWhileReadInFlight_waitsForBackendCall();
};

void TestFacadeThreading::constructDestroy_withoutUse_noThreadNoHang()
{
    QElapsedTimer t;
    t.start();
    {
        SerialPortActions serial; // never used: the I/O thread must not start
    }
    QVERIFY2(t.elapsed() < 1000, "unused facade must construct/destruct instantly");
}

void TestFacadeThreading::getSet_marshalsToBackendThread()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             {
                                 fake = new NiceFakeBackend();
                                 return fake;
                             });

    QVERIFY(serial.set_add_ssm_header(true)); // first call: starts the I/O thread
    QVERIFY(fake != nullptr);
    QVERIFY2(fake->thread() != QThread::currentThread(), "backend must live on the I/O thread, not the caller's");
    QCOMPARE(serial.get_add_ssm_header(), true);

    serial.set_serial_port_baudrate("10400");
    QCOMPARE(serial.get_serial_port_baudrate(), QString("10400"));
}

void TestFacadeThreading::scriptedRead_returnsThroughFacade()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             {
                                 fake = new NiceFakeBackend();
                                 return fake;
                             });

    serial.set_add_ssm_header(false); // force backend creation
    const QByteArray expected("\x80\xf0\x10\x02\xaa\xbb\x11", 7);
    EXPECT_CALL(*fake, read_serial_data(100)).WillOnce(::testing::Return(expected));

    QCOMPARE(serial.read_serial_data(100), expected);
}

void TestFacadeThreading::backendException_propagatesWithoutHangingAndCleansUp()
{
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("FASTECU_THROWING_BACKEND_CHILD", "1");
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(), {});
    QVERIFY2(child.waitForStarted(1000), qPrintable(child.errorString()));

    if (!child.waitForFinished(2000))
    {
        child.kill();
        child.waitForFinished(1000);
        QFAIL("backend exception left the facade caller blocked");
    }

    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
}

void TestFacadeThreading::transportAdapters_isOpenContainsBackendException()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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
        QFAIL("SSM standalone isOpen propagated a backend exception");
    }
    QVERIFY(!open);

    open = true;
    try
    {
        open = kline.isOpen();
    }
    catch (...)
    {
        QFAIL("K-Line standalone isOpen propagated a backend exception");
    }
    QVERIFY(!open);

    open = true;
    try
    {
        open = can.isOpen();
    }
    catch (...)
    {
        QFAIL("CAN standalone isOpen propagated a backend exception");
    }
    QVERIFY(!open);
}

void TestFacadeThreading::transportAdapters_normalEmptyReadIsSuccess()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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

    const auto ssmResult = ssm.read(10ms, cancellation);
    QVERIFY(ssmResult.has_value());
    QVERIFY(!ssmResult->has_value());

    const auto klineResult = kline.read(10ms, cancellation);
    QVERIFY(klineResult.has_value());
    QVERIFY(!klineResult->has_value());

    const auto canResult = can.read(10ms, cancellation);
    QVERIFY(canResult.has_value());
    QVERIFY(!canResult->has_value());
}

void TestFacadeThreading::transportAdapters_preCancelledReadSkipsBackend()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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

    const auto ssmResult = ssm.read(10ms, cancellation);
    QVERIFY(!ssmResult.has_value());
    QVERIFY(ssmResult.error().kind == fastecu::ErrorKind::Cancelled);

    const auto klineResult = kline.read(10ms, cancellation);
    QVERIFY(!klineResult.has_value());
    QVERIFY(klineResult.error().kind == fastecu::ErrorKind::Cancelled);

    const auto canResult = can.read(10ms, cancellation);
    QVERIFY(!canResult.has_value());
    QVERIFY(canResult.error().kind == fastecu::ErrorKind::Cancelled);
}

void TestFacadeThreading::transportAdapters_postCallCancellationPrecedesDisconnect()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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
    std::atomic<bool> portOpen{true};
    ON_CALL(*fake, is_serial_port_open()).WillByDefault([&portOpen] { return portOpen.load(); });
    EXPECT_CALL(*fake, read_serial_data(::testing::_))
        .WillRepeatedly(
            [&cancellation, &portOpen](std::uint16_t) -> QByteArray
            {
                cancellation.set_cancelled(true);
                portOpen.store(false);
                return QByteArray{};
            });

    const auto ssmResult = ssm.read(10ms, cancellation);
    QVERIFY(!ssmResult.has_value());
    QVERIFY(ssmResult.error().kind == fastecu::ErrorKind::Cancelled);

    cancellation.set_cancelled(false);
    portOpen.store(true);
    const auto klineResult = kline.read(10ms, cancellation);
    QVERIFY(!klineResult.has_value());
    QVERIFY(klineResult.error().kind == fastecu::ErrorKind::Cancelled);

    cancellation.set_cancelled(false);
    portOpen.store(true);
    const auto canResult = can.read(10ms, cancellation);
    QVERIFY(!canResult.has_value());
    QVERIFY(canResult.error().kind == fastecu::ErrorKind::Cancelled);
}

void TestFacadeThreading::transportAdapters_backendReadExceptionMapsToInternal()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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

    const auto ssmResult = ssm.read(10ms, cancellation);
    QVERIFY(!ssmResult.has_value());
    QVERIFY(ssmResult.error().kind == fastecu::ErrorKind::Internal);

    const auto klineResult = kline.read(10ms, cancellation);
    QVERIFY(!klineResult.has_value());
    QVERIFY(klineResult.error().kind == fastecu::ErrorKind::Internal);

    const auto canResult = can.read(10ms, cancellation);
    QVERIFY(!canResult.has_value());
    QVERIFY(canResult.error().kind == fastecu::ErrorKind::Internal);
}

void TestFacadeThreading::canTransport_truncatedFrameMapsToInternal()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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
    QVERIFY(!result.has_value());
    QVERIFY(result.error().kind == fastecu::ErrorKind::Internal);
}

void TestFacadeThreading::transportAdapters_nullOrClosedAdapterReturnsDisconnectedBeforeOperation()
{
    // A null serial pointer: adapters must fail without ever touching a backend.
    {
        FastEcuSsmTransport ssm(nullptr);
        mutdma::FastEcuKlineTransport kline(nullptr);
        cdbg::FastEcuCanTransport can(nullptr);
        fastecu::FakeCancellationToken cancellation;

        QVERIFY(!ssm.isOpen());
        QVERIFY(!kline.isOpen());
        QVERIFY(!can.isOpen());

        const auto ssmWrite = ssm.write(bytes::ByteView());
        QVERIFY(!ssmWrite.has_value());
        QVERIFY(ssmWrite.error().kind == fastecu::ErrorKind::Disconnected);

        const auto klineWrite = kline.write(bytes::ByteView());
        QVERIFY(!klineWrite.has_value());
        QVERIFY(klineWrite.error().kind == fastecu::ErrorKind::Disconnected);

        const auto klineBaud = kline.setBaud(10400);
        QVERIFY(!klineBaud.has_value());
        QVERIFY(klineBaud.error().kind == fastecu::ErrorKind::Disconnected);

        const auto canWrite = can.write(0x123, bytes::ByteView());
        QVERIFY(!canWrite.has_value());
        QVERIFY(canWrite.error().kind == fastecu::ErrorKind::Disconnected);

        const auto ssmRead = ssm.read(10ms, cancellation);
        QVERIFY(!ssmRead.has_value());
        QVERIFY(ssmRead.error().kind == fastecu::ErrorKind::Disconnected);

        const auto klineRead = kline.read(10ms, cancellation);
        QVERIFY(!klineRead.has_value());
        QVERIFY(klineRead.error().kind == fastecu::ErrorKind::Disconnected);

        const auto canRead = can.read(10ms, cancellation);
        QVERIFY(!canRead.has_value());
        QVERIFY(canRead.error().kind == fastecu::ErrorKind::Disconnected);
    }

    // A closed (but non-null) adapter: same Disconnected mapping, reached
    // through the backend's is_serial_port_open() rather than a null check.
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
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

        const auto ssmWrite = ssm.write(bytes::ByteView());
        QVERIFY(!ssmWrite.has_value());
        QVERIFY(ssmWrite.error().kind == fastecu::ErrorKind::Disconnected);

        const auto klineWrite = kline.write(bytes::ByteView());
        QVERIFY(!klineWrite.has_value());
        QVERIFY(klineWrite.error().kind == fastecu::ErrorKind::Disconnected);

        const auto klineBaud = kline.setBaud(10400);
        QVERIFY(!klineBaud.has_value());
        QVERIFY(klineBaud.error().kind == fastecu::ErrorKind::Disconnected);

        const auto canWrite = can.write(0x123, bytes::ByteView());
        QVERIFY(!canWrite.has_value());
        QVERIFY(canWrite.error().kind == fastecu::ErrorKind::Disconnected);

        const auto ssmRead = ssm.read(10ms, cancellation);
        QVERIFY(!ssmRead.has_value());
        QVERIFY(ssmRead.error().kind == fastecu::ErrorKind::Disconnected);

        const auto klineRead = kline.read(10ms, cancellation);
        QVERIFY(!klineRead.has_value());
        QVERIFY(klineRead.error().kind == fastecu::ErrorKind::Disconnected);

        const auto canRead = can.read(10ms, cancellation);
        QVERIFY(!canRead.has_value());
        QVERIFY(canRead.error().kind == fastecu::ErrorKind::Disconnected);
    }
}

void TestFacadeThreading::transportAdapters_writeSuccessAndCanFrameEncoding()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             {
                                 fake = new NiceFakeBackend();
                                 return fake;
                             });
    serial.set_add_ssm_header(false);
    FastEcuSsmTransport ssm(&serial);
    mutdma::FastEcuKlineTransport kline(&serial);
    cdbg::FastEcuCanTransport can(&serial);

    const bytes::Bytes ssmPayload{0x11, 0x22, 0x33};
    EXPECT_CALL(*fake, write_serial_data_echo_check(QByteArray::fromHex("112233")))
        .WillOnce(::testing::Return(QByteArray{}));
    const auto ssmResult = ssm.write(bytes::ByteView(ssmPayload));
    QVERIFY(ssmResult.has_value());
    QCOMPARE(*ssmResult, ssmPayload.size());

    const bytes::Bytes klinePayload{0xAA, 0xBB};
    EXPECT_CALL(*fake, write_serial_data(QByteArray::fromHex("aabb"))).WillOnce(::testing::Return(QByteArray{}));
    const auto klineResult = kline.write(bytes::ByteView(klinePayload));
    QVERIFY(klineResult.has_value());
    QCOMPARE(*klineResult, klinePayload.size());

    // CAN wire convention: 4 big-endian CAN-id bytes followed by the payload
    // verbatim -- confirm the adapter builds exactly that frame, byte for byte.
    const bytes::Bytes canPayload{0xDE, 0xAD, 0xBE, 0xEF};
    QByteArray expectedFrame;
    bytes::appendU32Be(expectedFrame, 0x123);
    expectedFrame.append(bytes::toQByteArray(bytes::ByteView(canPayload)));
    EXPECT_CALL(*fake, write_serial_data_echo_check(expectedFrame)).WillOnce(::testing::Return(QByteArray{}));
    const auto canResult = can.write(0x123, bytes::ByteView(canPayload));
    QVERIFY(canResult.has_value());
    QCOMPARE(*canResult, canPayload.size());
}

void TestFacadeThreading::transportAdapters_disconnectDuringWriteMapsToDisconnected()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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

    const auto ssmResult = ssm.write(bytes::ByteView());
    QVERIFY(!ssmResult.has_value());
    QVERIFY(ssmResult.error().kind == fastecu::ErrorKind::Disconnected);

    const auto klineResult = kline.write(bytes::ByteView());
    QVERIFY(!klineResult.has_value());
    QVERIFY(klineResult.error().kind == fastecu::ErrorKind::Disconnected);

    const auto canResult = can.write(0x123, bytes::ByteView());
    QVERIFY(!canResult.has_value());
    QVERIFY(canResult.error().kind == fastecu::ErrorKind::Disconnected);
}

void TestFacadeThreading::transportAdapters_disconnectDuringReadMapsToDisconnected()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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

    const auto ssmResult = ssm.read(10ms, cancellation);
    QVERIFY(!ssmResult.has_value());
    QVERIFY(ssmResult.error().kind == fastecu::ErrorKind::Disconnected);

    const auto klineResult = kline.read(10ms, cancellation);
    QVERIFY(!klineResult.has_value());
    QVERIFY(klineResult.error().kind == fastecu::ErrorKind::Disconnected);

    const auto canResult = can.read(10ms, cancellation);
    QVERIFY(!canResult.has_value());
    QVERIFY(canResult.error().kind == fastecu::ErrorKind::Disconnected);
}

void TestFacadeThreading::transportAdapters_backendWriteExceptionMapsToInternal()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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

    const auto ssmResult = ssm.write(bytes::ByteView());
    QVERIFY(!ssmResult.has_value());
    QVERIFY(ssmResult.error().kind == fastecu::ErrorKind::Internal);

    const auto klineResult = kline.write(bytes::ByteView());
    QVERIFY(!klineResult.has_value());
    QVERIFY(klineResult.error().kind == fastecu::ErrorKind::Internal);

    const auto canResult = can.write(0x123, bytes::ByteView());
    QVERIFY(!canResult.has_value());
    QVERIFY(canResult.error().kind == fastecu::ErrorKind::Internal);
}

void TestFacadeThreading::transportAdapters_backendNonStandardExceptionMapsToInternal()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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
    const auto ssmRead = ssm.read(10ms, cancellation);
    QVERIFY(!ssmRead.has_value());
    QVERIFY(ssmRead.error().kind == fastecu::ErrorKind::Internal);

    const auto klineRead = kline.read(10ms, cancellation);
    QVERIFY(!klineRead.has_value());
    QVERIFY(klineRead.error().kind == fastecu::ErrorKind::Internal);

    const auto canRead = can.read(10ms, cancellation);
    QVERIFY(!canRead.has_value());
    QVERIFY(canRead.error().kind == fastecu::ErrorKind::Internal);

    const auto ssmWrite = ssm.write(bytes::ByteView());
    QVERIFY(!ssmWrite.has_value());
    QVERIFY(ssmWrite.error().kind == fastecu::ErrorKind::Internal);

    const auto klineWrite = kline.write(bytes::ByteView());
    QVERIFY(!klineWrite.has_value());
    QVERIFY(klineWrite.error().kind == fastecu::ErrorKind::Internal);

    const auto canWrite = can.write(0x123, bytes::ByteView());
    QVERIFY(!canWrite.has_value());
    QVERIFY(canWrite.error().kind == fastecu::ErrorKind::Internal);
}

void TestFacadeThreading::transportAdapters_cancellationPrecedesReadException()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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

    const auto ssmResult = ssm.read(10ms, cancellation);
    QVERIFY(!ssmResult.has_value());
    QVERIFY(ssmResult.error().kind == fastecu::ErrorKind::Cancelled);

    cancellation.set_cancelled(false);
    const auto klineResult = kline.read(10ms, cancellation);
    QVERIFY(!klineResult.has_value());
    QVERIFY(klineResult.error().kind == fastecu::ErrorKind::Cancelled);

    cancellation.set_cancelled(false);
    const auto canResult = can.read(10ms, cancellation);
    QVERIFY(!canResult.has_value());
    QVERIFY(canResult.error().kind == fastecu::ErrorKind::Cancelled);
}

void TestFacadeThreading::klineTransport_setBaudSuccessRejectionDisconnectException()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             {
                                 fake = new NiceFakeBackend();
                                 return fake;
                             });
    serial.set_add_ssm_header(false);
    mutdma::FastEcuKlineTransport kline(&serial);

    ::testing::InSequence sequence;
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, change_port_speed(QString("10400"))).WillOnce(::testing::Return(STATUS_SUCCESS));
    const auto success = kline.setBaud(10400);
    QVERIFY(success.has_value());

    // Rejection: driver returns non-zero but the port stays open.
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, change_port_speed(QString("10400"))).WillOnce(::testing::Return(STATUS_ERROR));
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    const auto rejected = kline.setBaud(10400);
    QVERIFY(!rejected.has_value());
    QVERIFY(rejected.error().kind == fastecu::ErrorKind::Internal);

    // Disconnect: driver returns non-zero and the port is found closed
    // immediately afterward.
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, change_port_speed(QString("10400"))).WillOnce(::testing::Return(STATUS_ERROR));
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
    const auto disconnected = kline.setBaud(10400);
    QVERIFY(!disconnected.has_value());
    QVERIFY(disconnected.error().kind == fastecu::ErrorKind::Disconnected);
    // Exception: driver throws instead of returning.
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, change_port_speed(QString("10400")))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend baud-change failure")));
    const auto thrown = kline.setBaud(10400);
    QVERIFY(!thrown.has_value());
    QVERIFY(thrown.error().kind == fastecu::ErrorKind::Internal);

    // Non-standard exception: still mapped to Internal via catch(...).
    EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(*fake, change_port_speed(QString("10400"))).WillOnce(ThrowNonStandardBackendFailure());
    const auto thrownNonStandard = kline.setBaud(10400);
    QVERIFY(!thrownNonStandard.has_value());
    QVERIFY(thrownNonStandard.error().kind == fastecu::ErrorKind::Internal);
}

// ---- affinity-warning capture ------------------------------------------
static QStringList g_threadWarnings;
static QtMessageHandler g_prevHandler = nullptr;

static void warningCapture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (type == QtWarningMsg && (msg.contains("another thread") || msg.contains("different thread")))
    {
        g_threadWarnings.append(msg);
    }
    if (g_prevHandler)
    {
        g_prevHandler(type, ctx, msg);
    }
}

void TestFacadeThreading::workerThreadCaller_noAffinityWarnings()
{
    // The exact LoggingWorker scenario from the bench checklist: a non-GUI
    // thread drives the facade. Data must arrive and Qt must emit no
    // cross-thread affinity warnings.
    g_threadWarnings.clear();
    g_prevHandler = qInstallMessageHandler(warningCapture);

    const QByteArray expected("\x80\xf0\x10\x01\x55\x66", 6);
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
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

    qInstallMessageHandler(g_prevHandler);
    QCOMPARE(got, expected);
    QVERIFY2(g_threadWarnings.isEmpty(), qPrintable("affinity warnings: " + g_threadWarnings.join(" | ")));
}

void TestFacadeThreading::concurrentCallers_serializeWithoutInterleaving()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend *
                             {
                                 fake = new NiceFakeBackend();
                                 return fake;
                             });
    serial.set_add_ssm_header(false); // create backend

    std::atomic<int> activeCalls{0};
    std::atomic<bool> interleaved{false};
    auto serializedCall = [&activeCalls, &interleaved](auto&&) -> QByteArray
    {
        if (activeCalls.fetch_add(1) != 0)
        {
            interleaved.store(true);
        }
        QThread::msleep(20);
        activeCalls.fetch_sub(1);
        return {};
    };
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(10).WillRepeatedly(serializedCall);
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(10).WillRepeatedly(serializedCall);

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

    QVERIFY(!interleaved.load());
    QCOMPARE(activeCalls.load(), 0);
}

void TestFacadeThreading::destroyAfterUse_joinsIoThread()
{
    QPointer<QThread> ioThread;
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 {
                                     fake = new NiceFakeBackend();
                                     return fake;
                                 });
        serial.set_add_ssm_header(true);
        ioThread = fake->thread();
        QVERIFY(ioThread && ioThread->isRunning());
    }
    QVERIFY2(!ioThread || !ioThread->isRunning(), "facade destruction must stop and join the I/O thread");
}

void TestFacadeThreading::destroyWhileReadInFlight_waitsForBackendCall()
{
    FakeBackend *fake = nullptr;
    auto *serial = new SerialPortActions("", "", nullptr, nullptr,
                                         [&fake]() -> SerialBackend *
                                         {
                                             fake = new NiceFakeBackend();
                                             return fake;
                                         });

    serial->set_add_ssm_header(false); // create backend before wiring gates

    QSemaphore readEntered;
    QSemaphore continueRead;
    EXPECT_CALL(*fake, read_serial_data(10))
        .WillOnce(
            [&readEntered, &continueRead](std::uint16_t)
            {
                readEntered.release();
                continueRead.acquire();
                return QByteArray("done");
            });

    QByteArray got;
    std::thread reader([&] { got = serial->read_serial_data(10); });
    QVERIFY2(readEntered.tryAcquire(1, 1000), "backend read did not start");

    std::atomic<bool> destroyed{false};
    std::thread destroyer(
        [&]
        {
            delete serial;
            destroyed.store(true);
        });

    QTest::qWait(50);
    QVERIFY2(!destroyed.load(), "facade teardown must wait for the in-flight backend call");

    continueRead.release();
    reader.join();
    destroyer.join();

    QCOMPARE(got, QByteArray("done"));
    QVERIFY(destroyed.load());
}

int run_test_facade_threading(int argc, char **argv)
{
    TestFacadeThreading t;
    const int result = QTest::qExec(&t, argc, argv);
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}

int run_throwing_backend_child()
{
    bool backendDestroyed = false;
    bool exceptionPropagated = false;
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 {
                                     fake = new NiceFakeBackend();
                                     return fake;
                                 });
        serial.set_add_ssm_header(false);
        EXPECT_CALL(*fake, read_serial_data(10))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend read failure")));
        fake->destroyed = &backendDestroyed;

        try
        {
            serial.read_serial_data(10);
        }
        catch (const std::runtime_error& error)
        {
            exceptionPropagated = QString::fromUtf8(error.what()) == QStringLiteral("scripted backend read failure");
        }
    }
    return exceptionPropagated && backendDestroyed && !::testing::Test::HasFailure() ? 0 : 1;
}

#include "facade_threading_test.moc"
