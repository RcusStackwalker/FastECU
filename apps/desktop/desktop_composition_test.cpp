#include <QElapsedTimer>
#include <QScopeGuard>
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
#include "apps/desktop/startup_diagnostics.h"
#include "src/backend/definitions/file_actions.h"

#include <QDir>
#include <QFile>
#include <QSemaphore>
#include <QSignalSpy>

#include <algorithm>
#include <memory>

#include "src/platform/desktop/common/logging/systemlogger.h"
#include "src/platform/desktop/common/remote_utility/remote_utility.h"
#include "src/ui/desktop/channels/log_channel.h"
#include "src/ui/desktop/channels/remote_peer.h"

namespace
{

using fastecu::ui::LogChannel;
using fastecu::ui::RemotePeer;

constexpr auto kVersion = "0.1.0-beta.5";

bool writeFile(const QString& path, const QString& text)
{
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    const QByteArray bytes = text.toUtf8();
    return file.write(bytes) == bytes.size();
}

// Holds the syslog thread inside one queued call until release(), so a test
// can emit and destroy senders before the logger sees their lines.
class SyslogGate
{
  public:
    explicit SyslogGate(SystemLogger& logger)
    {
        QMetaObject::invokeMethod(
            &logger,
            [this]
            {
                entered_.release();
                open_.acquire();
            },
            Qt::QueuedConnection);
        entered_.acquire();
    }
    ~SyslogGate()
    {
        release();
    }
    SyslogGate(const SyslogGate&) = delete;
    SyslogGate& operator=(const SyslogGate&) = delete;

    void release()
    {
        if (!released_)
        {
            released_ = true;
            open_.release();
        }
    }

  private:
    QSemaphore entered_;
    QSemaphore open_;
    bool released_ = false;
};

bool has_line_ending_with(const QSignalSpy& spy, const QString& suffix)
{
    return std::ranges::any_of(spy, [&](const QList<QVariant>& arguments)
                               { return arguments.at(0).toString().endsWith(suffix); });
}

bool has_line_containing(const QSignalSpy& spy, const QString& text)
{
    return std::ranges::any_of(spy, [&](const QList<QVariant>& arguments)
                               { return arguments.at(0).toString().contains(text); });
}

} // namespace

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
        QCOMPARE(&first.log, &second.log);
        QCOMPARE(&first.connection, &second.connection);
        QCOMPARE(&first.remote, &second.remote);
        QCOMPARE(&first.logging_engine, &second.logging_engine);
        QCOMPARE(&first.config, &second.config);
        QCOMPARE(&first.application, &second.application);
    }

    void failedStartupBuildsNoServicesAndPerformsNoEcuIo()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString config_dir = root.path() + "/" + kVersion + "/config/";
        QVERIFY(QDir().mkpath(config_dir));
        QVERIFY(
            writeFile(config_dir + "protocols.cfg", R"(<config name="FastECU"><protocols/><car_models/></config>)"));

        DesktopComposition composition{{}, {}, root.path()};

        QVERIFY(!composition.started());
        QVERIFY(composition.startup_error().has_value());
        QVERIFY(QString::fromStdString(composition.startup_error()->detail).contains(config_dir + "protocols.cfg"));
        // Nothing that could log, thread, or talk to an ECU was created.
        QVERIFY(!composition.file_actions_);
        QVERIFY(!composition.syslog_thread_);
        QVERIFY(!composition.syslogger_);
        QVERIFY(!composition.serial_);
        QVERIFY(!composition.connection_);
        QVERIFY(!composition.remote_utility_);
        QVERIFY(!composition.logging_engine_);
    } // teardown after a failed start must not crash

    void migrationLoadsPreviousVersionSettingsFromDisk()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString previous_dir = root.path() + "/0.1.0-beta.4/config/";
        QVERIFY(QDir().mkpath(previous_dir));
        QVERIFY(writeFile(previous_dir + "fastecu.cfg",
                          R"(<config name="FastECU"><software_settings>
<setting name="serial_port"><value data="ttyMIGRATED_UNIQUE"/></setting>
</software_settings></config>)"));
        const QString current_file = root.path() + "/" + kVersion + "/config/fastecu.cfg";
        QVERIFY(!QFile::exists(current_file));

        DesktopComposition composition{{}, {}, root.path()};

        QVERIFY(composition.started());
        QCOMPARE(composition.config_.settings().serial_port, std::string("ttyMIGRATED_UNIQUE"));
        QFile saved{current_file};
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.readAll().contains("ttyMIGRATED_UNIQUE"));
    }

    void malformedSettingsRejectStartup()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString config_dir = root.path() + "/" + kVersion + "/config/";
        QVERIFY(QDir().mkpath(config_dir));
        QVERIFY(writeFile(config_dir + "fastecu.cfg", "<config"));

        DesktopComposition composition{{}, {}, root.path()};

        QVERIFY(!composition.started());
        const QString text = startup_failure_text(*composition.startup_error());
        QVERIFY(text.contains(config_dir + "fastecu.cfg"));
        QVERIFY(!composition.serial_);
    }

    void settingsRewriteFailureIsAStartupWarning()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString config_dir = root.path() + "/" + kVersion + "/config/";
        QVERIFY(QDir().mkpath(config_dir));
        const QString config_file = config_dir + "fastecu.cfg";
        QVERIFY(writeFile(config_file, R"(<config name="FastECU" version="t"><software_settings/></config>)"));
        QVERIFY(QFile::setPermissions(config_file, QFileDevice::ReadOwner));
        const auto restore =
            qScopeGuard([&] { QFile::setPermissions(config_file, QFileDevice::ReadOwner | QFileDevice::WriteOwner); });
        if (QFile probe{config_file}; probe.open(QIODevice::WriteOnly | QIODevice::Append))
        {
            QSKIP("this filesystem ignores the read-only permission");
        }

        DesktopComposition composition{{}, {}, root.path()};

        QVERIFY(composition.started());
        QVERIFY(std::ranges::any_of(composition.startup_warnings(),
                                    [&](const QString& warning) { return warning.contains(config_file); }));
    }

    void servicesShareTheCompositionsSession()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        QVERIFY(composition.started());
        QCOMPARE(&composition.services().config, &composition.config_);
        QCOMPARE(composition.services().application.version, std::string(kVersion));
        QCOMPARE(QString::fromStdString(composition.config_.provisioned_paths().base_config_directory), root.path());
    }

    void restartSeesSavedSettingsButNotTheDatalogDirectory()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        std::string provisioned_datalogs;
        {
            DesktopComposition first{{}, {}, root.path()};
            QVERIFY(first.started());
            provisioned_datalogs = first.config_.provisioned_paths().datalog_files_directory;
            first.config_.settings().serial_port = "ttyRESTART";
            first.config_.settings().datalog_files_directory = root.path().toStdString() + "/elsewhere/";
            QVERIFY(first.config_.save().has_value());
        }
        DesktopComposition second{{}, {}, root.path()};
        QVERIFY(second.started());
        QCOMPARE(second.config_.settings().serial_port, std::string("ttyRESTART"));
        QCOMPARE(second.config_.settings().datalog_files_directory, provisioned_datalogs);
    }

    void waitRequestIsWiredToTheRemoteUtility()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        // Running the wait needs a peer; it loops until one answers, so this
        // checks the wiring without invoking it: isSignalConnected is
        // protected, so disconnect-and-report-whether-anything-was-there
        // is the public way to observe the same fact.
        QVERIFY(QObject::disconnect(&composition.services().remote, &RemotePeer::wait_requested, nullptr, nullptr));
    }

    void remoteStateChangesReachThePeer()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        QSignalSpy changes{&composition.services().remote, &RemotePeer::stateChanged};

        emit composition.remote_utility_->stateChanged(QRemoteObjectReplica::Suspect, QRemoteObjectReplica::Valid);

        QCOMPARE(changes.count(), 1);
        QCOMPARE(changes.at(0).at(0).value<QRemoteObjectReplica::State>(), QRemoteObjectReplica::Suspect);
        QCOMPARE(changes.at(0).at(1).value<QRemoteObjectReplica::State>(), QRemoteObjectReplica::Valid);
    }

    // No --host: the replica never becomes valid, so the mirror drops both.
    void mirroringWithoutAPeerReturnsPromptly()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        RemotePeer& remote = composition.services().remote;
        QVERIFY(!composition.remote_utility_->isValid());

        QElapsedTimer elapsed;
        elapsed.start();
        emit remote.log_window_message("mirrored line");
        emit remote.progress(42);
        QVERIFY2(elapsed.elapsed() < 1000, "mirroring without a peer blocked");
    }

    void channelLevelsReachTheLogWindowWithTheirPrefix()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        LogChannel& log = composition.services().log;
        QSignalSpy window{&log, &LogChannel::log_window_message};

        emit log.LOG_E("error line", true, false);
        emit log.LOG_W("warning line", true, false);
        emit log.LOG_I("info line", true, false);

        // Match by content: the logger's own "SystemLogger started..." line
        // can reach the window too.
        QTRY_VERIFY(has_line_ending_with(window, "(II) info line"));
        QVERIFY(has_line_ending_with(window, "(EE) error line"));
        QVERIFY(has_line_ending_with(window, "(WW) warning line"));
    }

    void debugLinesStayOutOfTheLogWindow()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        LogChannel& log = composition.services().log;
        QSignalSpy window{&log, &LogChannel::log_window_message};

        emit log.LOG_D("debug line", true, false);
        emit log.LOG_I("sentinel", false, false);

        QTRY_VERIFY(has_line_ending_with(window, "sentinel"));
        QVERIFY(!has_line_containing(window, "debug line"));
    }

    // A dialog that logs and is destroyed before the syslog thread delivers
    // its line: through the channel the line survives; connected straight to
    // the logger, as UI code did before step 6j, it is dropped.
    void relayedLineSurvivesItsSenderButADirectOneDoesNot()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        LogChannel& log = composition.services().log;
        QSignalSpy window{&log, &LogChannel::log_window_message};

        {
            SyslogGate gate{*composition.syslogger_};
            auto relayed = std::make_unique<LogChannel>();
            QObject::connect(relayed.get(), &LogChannel::LOG_I, &log, &LogChannel::LOG_I);
            auto direct = std::make_unique<LogChannel>();
            QObject::connect(direct.get(), &LogChannel::LOG_I, composition.syslogger_.get(),
                             &SystemLogger::log_messages);

            emit relayed->LOG_I("relayed line", false, false);
            emit direct->LOG_I("direct line", false, false);
            relayed.reset();
            direct.reset();
        }
        emit log.LOG_I("sentinel", false, false);

        QTRY_VERIFY(has_line_ending_with(window, "sentinel"));
        QVERIFY(has_line_ending_with(window, "relayed line"));
        QVERIFY(!has_line_containing(window, "direct line"));
    }

    void enablingFileLoggingWritesASyslogFile()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        const QString syslog_dir =
            QString::fromStdString(composition.services().config.effective_paths().syslog_files_directory);
        QVERIFY(!syslog_dir.isEmpty());
        QVERIFY(QDir().mkpath(syslog_dir));
        LogChannel& log = composition.services().log;
        QSignalSpy window{&log, &LogChannel::log_window_message};

        emit log.enable_log_write_to_file(true);
        emit log.LOG_I("to file", false, true);
        emit log.LOG_D("debug to file", false, true);
        // log_messages signals the window before it writes the file; the
        // sentinel's window line proves the earlier write has finished.
        emit log.LOG_I("sentinel", false, false);

        QTRY_VERIFY(has_line_ending_with(window, "sentinel"));
        const QStringList files = QDir(syslog_dir).entryList({"log_fastecu_*.txt"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QFile file{QDir(syslog_dir).filePath(files.first())};
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray contents = file.readAll();
        QVERIFY(contents.contains("to file"));
        QVERIFY(contents.contains("debug to file"));
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
            QVERIFY(composition.started());
            QCOMPARE(QString::fromStdString(composition.services().config.provisioned_paths().base_config_directory),
                     root.path());
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
