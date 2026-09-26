#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <variant>
#include <QMap>
#include "src/platform/desktop/common/logging/logging_worker.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#define private public
#include "src/platform/desktop/common/logging/logging_engine.h"
#undef private

#include "apps/desktop/desktop_composition.h"
#include "src/backend/definitions/file_actions.h"

class DesktopCompositionTest : public QObject
{
    Q_OBJECT

  private slots:
    void compositionRegistersAllLoggingProtocolsWithoutWindow()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        QCOMPARE(composition.services().logging_engine.registrations_.keys(), (QStringList{"CDBG", "MUT_DMA", "SSM"}));
    }

    void servicesReferToTheCompositionsOwnObjects()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};

        const MainWindowServices first = composition.services();
        const MainWindowServices second = composition.services();

        QCOMPARE(&first.file_actions, &second.file_actions);
        QCOMPARE(&first.config_repository, &second.config_repository);
        QCOMPARE(&first.file_action_events, &second.file_action_events);
        QCOMPARE(&first.syslogger, &second.syslogger);
        QCOMPARE(&first.serial, &second.serial);
        QCOMPARE(&first.remote_utility, &second.remote_utility);
        QCOMPARE(&first.logging_engine, &second.logging_engine);
        QCOMPARE(first.file_actions.ConfigValuesStruct.base_config_directory, root.path());
    }

    // SystemLogger::run() spends its first second in a processEvents loop on
    // the syslog thread; the destructor must still stop and join it promptly.
    void destructionRightAfterConstructionDoesNotHang()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QElapsedTimer elapsed;
        elapsed.start();
        {
            DesktopComposition composition{{}, {}, root.path()};
        }
        QVERIFY2(elapsed.elapsed() < 5000, "composition teardown took longer than 5 s");
    }

    // main() rebuilds the composition on every RESTART_CODE iteration.
    void constructingTwiceInOneProcessSucceeds()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        for (int iteration = 0; iteration < 2; ++iteration)
        {
            DesktopComposition composition{{}, {}, root.path()};
            QCOMPARE(composition.services().file_actions.ConfigValuesStruct.base_config_directory, root.path());
        }
    }

    void emptyHostSelectsTheDirectBackend()
    {
        QVERIFY(std::holds_alternative<DirectSerial>(serial_connection_from_args({}, {})));
        QVERIFY(std::holds_alternative<DirectSerial>(serial_connection_from_args({}, "ignored")));
    }

    void nonEmptyHostSelectsTheRemoteBackendWithItsCredentials()
    {
        const SerialConnection connection = serial_connection_from_args("peer.example:1234", "secret");
        const auto *remote = std::get_if<RemoteSerial>(&connection);
        QVERIFY(remote != nullptr);
        QCOMPARE(remote->address, QString("peer.example:1234"));
        QCOMPARE(remote->password, QString("secret"));
    }
};

QTEST_GUILESS_MAIN(DesktopCompositionTest)
#include "desktop_composition_test.moc"
