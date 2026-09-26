#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

class FakeBackendTest : public QObject
{
    Q_OBJECT

  private slots:
    void defaultActionsPreserveConfigurationThroughFacade()
    {
        SerialPortActions serial([]() -> SerialBackend * { return new NiceFakeBackend; });
        QVERIFY(serial.set_add_iso14230_header(true));
        QVERIFY(serial.set_can_source_address(0x7E1));
        QVERIFY(serial.set_serial_port_baudrate("10400"));
        QCOMPARE(serial.get_add_iso14230_header(), true);
        QCOMPARE(serial.get_can_source_address(), std::uint32_t{0x7E1});
        QCOMPARE(serial.get_serial_port_baudrate(), QStringLiteral("10400"));
        // Hardware operations have inert defaults, even without a selected port.
        QCOMPARE(serial.open_serial_port(), QString{});
        QCOMPARE(serial.read_serial_data(10), QByteArray{});
    }

    void expectationsScriptFacadeIoInOrder()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial(
            [&fake]() -> SerialBackend *
            {
                fake = new NiceFakeBackend;
                return fake;
            });
        QVERIFY(serial.set_add_ssm_header(false)); // create backend before expectations
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, write_serial_data(QByteArray("request"))).WillOnce(::testing::Return(QByteArray("request")));
        EXPECT_CALL(*fake, read_serial_data(50)).WillOnce(::testing::Return(QByteArray("reply")));
        QCOMPARE(serial.write_serial_data("request"), QByteArray("request"));
        QCOMPARE(serial.read_serial_data(50), QByteArray("reply"));
    }

    void expectationFailuresProduceNonzeroExit()
    {
        const QString mode = qEnvironmentVariable("FASTECU_GMOCK_FAILURE_PROBE");
        if (!mode.isEmpty())
        {
            FakeBackend *fake = nullptr;
            SerialPortActions serial(
                [&fake]() -> SerialBackend *
                {
                    fake = new NiceFakeBackend;
                    return fake;
                });
            QVERIFY(serial.set_add_ssm_header(false));
            if (mode == "unmet")
            {
                EXPECT_CALL(*fake, read_serial_data(50)).Times(1);
            }
            else if (mode == "forbidden")
            {
                EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
                serial.read_serial_data(50);
            }
            else
            {
                EXPECT_CALL(*fake, read_serial_data(50)).Times(1);
                serial.read_serial_data(60);
            }
            return; // mock destruction verifies expectations on the facade's I/O thread
        }

        for (const QString& probe :
             {QStringLiteral("unmet"), QStringLiteral("forbidden"), QStringLiteral("wrong-argument")})
        {
            QProcess child;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("FASTECU_GMOCK_FAILURE_PROBE", probe);
            child.setProcessEnvironment(environment);
            child.setProcessChannelMode(QProcess::MergedChannels);
            child.start(QCoreApplication::applicationFilePath(), {"expectationFailuresProduceNonzeroExit"});
            QVERIFY2(child.waitForStarted(5000), qPrintable(child.errorString()));
            QVERIFY2(child.waitForFinished(10000), qPrintable(child.errorString()));
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QCOMPARE(child.exitCode(), 1);
            // GoogleTest labels a failed assertion "Failure", except under
            // MSVC, where it emits Visual Studio's "error: " instead
            // (TestPartResultTypeToString in googletest/src/gtest.cc). Matching
            // only "Failure" therefore never matches on Windows. Accept either
            // spelling, and pin the check to the mocked call the diagnostic has
            // to name, so this still proves GMock reported *this* expectation
            // rather than that some incidental text was printed.
            const QByteArray diagnostics = child.readAll();
            QVERIFY2(diagnostics.contains("Failure") || diagnostics.contains("error: "), diagnostics.constData());
            QVERIFY2(diagnostics.contains("read_serial_data"), diagnostics.constData());
        }
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QCoreApplication application(argc, argv);
    FakeBackendTest test;
    const int result = QTest::qExec(&test, argc, argv);
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}

#include "fake_backend_test.moc"
