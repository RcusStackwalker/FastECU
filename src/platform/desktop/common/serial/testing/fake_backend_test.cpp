#include <cstdio>
#include <vector>

#include <QDir>

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
            const QString exe = QCoreApplication::applicationFilePath();
            fprintf(stderr, "PROBE 30 probe=%s exe=%s\n", qUtf8Printable(probe), qUtf8Printable(exe));
            fflush(stderr);
            child.start(exe, {"expectationFailuresProduceNonzeroExit"});
            const bool started = child.waitForStarted(5000);
            fprintf(stderr, "PROBE 31 started=%d error=%d errorString=%s\n", static_cast<int>(started),
                    static_cast<int>(child.error()), qUtf8Printable(child.errorString()));
            fflush(stderr);
            QVERIFY2(started, qPrintable(child.errorString()));
            const bool finished = child.waitForFinished(10000);
            const QByteArray out = child.readAll();
            fprintf(stderr, "PROBE 32 finished=%d exitStatus=%d exitCode=%d outBytes=%d\n", static_cast<int>(finished),
                    static_cast<int>(child.exitStatus()), child.exitCode(), static_cast<int>(out.size()));
            fprintf(stderr, "PROBE 33 childOutput<<<%.600s>>>\n", out.constData());
            fflush(stderr);
            QVERIFY2(finished, qPrintable(child.errorString()));
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QCOMPARE(child.exitCode(), 1);
            QVERIFY(out.contains("Failure"));
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

    // TEMPORARY: Bazel only prints a test's log when it fails, and QtTest's own
    // output has never appeared in test.log on Windows. Route QtTest to a file
    // and echo that file to stderr (which demonstrably survives), so the real
    // banner/PASS/FAIL lines are visible regardless of how stdout is routed.
    const QByteArray logSpec =
        QDir::temp()
            .filePath(QStringLiteral("fastecu_qtest_log_%1.txt").arg(QCoreApplication::applicationPid()))
            .toLocal8Bit();
    QByteArray oFlag("-o");
    QByteArray oSpec = logSpec + ",txt";
    std::vector<char *> qtArgs(argv, argv + argc);
    qtArgs.push_back(oFlag.data());
    qtArgs.push_back(oSpec.data());
    int qtArgc = static_cast<int>(qtArgs.size());
    death_probe("PROBE 04 fixture constructed, entering qExec");
    const int result = QTest::qExec(&test, qtArgc, qtArgs.data());
    {
        fprintf(stderr, "PROBE 06 ---- QtTest log begins ----\n");
        FILE *f = fopen(logSpec.constData(), "rb");
        if (!f)
        {
            fprintf(stderr, "PROBE 06 could not reopen QtTest log\n");
        }
        else
        {
            char buf[4096];
            size_t n = 0;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            {
                fwrite(buf, 1, n, stderr);
            }
            fclose(f);
        }
        fprintf(stderr, "\nPROBE 06 ---- QtTest log ends ----\n");
        fflush(stderr);
    }
    fprintf(stderr, "PROBE 05 qExec returned %d hasFailure=%d stdout_err=%d\n", result,
            static_cast<int>(::testing::Test::HasFailure()), ferror(stdout));
    fflush(stderr);
    // TEMPORARY: always fail so Bazel prints this log on every Windows run,
    // making the flaky failure observable without waiting to win a coin flip.
    fprintf(stderr, "PROBE 07 forcing nonzero exit for diagnostics (real result=%d)\n",
            result != 0 || ::testing::Test::HasFailure() ? 1 : 0);
    fflush(stderr);
    return 1;
}

#include "fake_backend_test.moc"
