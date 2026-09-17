#include <cstdio>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

// TEMPORARY Windows death-point instrumentation (removed before merge).
// Deliberately minimal: static init plus main only, no probes inside the
// slots, to perturb codegen as little as possible while still reporting how
// far the process gets.
static void death_probe(const char *what)
{
    fputs(what, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

static struct StaticInitProbe
{
    StaticInitProbe()
    {
        death_probe("PROBE 00 static-init (pre-main)");
    }
} g_static_init_probe;

class FakeBackendTest : public QObject
{
    Q_OBJECT

  private slots:
    void defaultActionsPreserveConfigurationThroughFacade()
    {
        SerialPortActions serial("", "", nullptr, nullptr, []() -> SerialBackend * { return new NiceFakeBackend; });
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
        SerialPortActions serial("", "", nullptr, nullptr,
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
            SerialPortActions serial("", "", nullptr, nullptr,
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
            QVERIFY(child.readAll().contains("Failure"));
        }
    }
};

int main(int argc, char **argv)
{
    // Unbuffered diagnostics, as in every other serial suite's main (see
    // facade_threading_main.cpp and the P0 entry in docs/tech-debt.md). This
    // suite exercises the facade's I/O thread, which carries an intermittent
    // Windows-only crash. Bazel redirects stdout to test.log, where it is
    // block-buffered, so a hard crash discards the whole buffer and the run
    // reports zero output -- leaving the failing slot unidentifiable.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    death_probe("PROBE 01 main enter");
    // Is stdout itself reaching test.log? QTest logs to stdout; the probes
    // above use stderr. If this line is absent while the stderr probes show,
    // stdout is being lost and QTest's output never had a chance.
    fputs("PROBE 01b stdout reachable\n", stdout);
    fflush(stdout);
    fprintf(stderr, "PROBE 01c stdout fileno=%d ferror=%d\n", fileno(stdout), ferror(stdout));
    fflush(stderr);

    ::testing::InitGoogleMock(&argc, argv);
    death_probe("PROBE 02 InitGoogleMock done");
    QCoreApplication application(argc, argv);
    death_probe("PROBE 03 QCoreApplication constructed");
    FakeBackendTest test;
    death_probe("PROBE 04 fixture constructed, entering qExec");
    const int result = QTest::qExec(&test, argc, argv);
    fprintf(stderr, "PROBE 05 qExec returned %d hasFailure=%d stdout_err=%d\n", result,
            static_cast<int>(::testing::Test::HasFailure()), ferror(stdout));
    fflush(stderr);
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}

#include "fake_backend_test.moc"
