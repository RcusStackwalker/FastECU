#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <variant>
#include <QMap>
#include "src/platform/desktop/common/logging/logging_worker.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#define private public
#include "src/platform/desktop/common/logging/logging_engine.h"
#undef private

#include "apps/desktop/desktop_composition.h"
#include "apps/desktop/startup_diagnostics.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/platform/desktop/common/definition/definition_catalog_session.h"

#include <QDir>
#include <QFile>
#include <QSemaphore>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"

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
//
// The semaphores are shared with the queued call: after release() the syslog
// thread may still be waking inside open.acquire(), so they must outlive the
// gate rather than die with it.
class SyslogGate
{
  public:
    explicit SyslogGate(SystemLogger& logger)
    {
        QMetaObject::invokeMethod(
            &logger,
            [state = state_]
            {
                state->entered.release();
                state->open.acquire();
            },
            Qt::QueuedConnection);
        state_->entered.acquire();
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
            state_->open.release();
        }
    }

  private:
    struct State
    {
        QSemaphore entered;
        QSemaphore open;
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();
    bool released_ = false;
};

bool has_line_ending_with(const auto& spy, const QString& suffix)
{
    return std::ranges::any_of(spy.snapshot(),
                               [&](const auto& arguments) { return std::get<0>(arguments).endsWith(suffix); });
}

bool has_line_containing(const auto& spy, const QString& text)
{
    return std::ranges::any_of(spy.snapshot(),
                               [&](const auto& arguments) { return std::get<0>(arguments).contains(text); });
}

} // namespace

class DesktopCompositionTest : public ::testing::Test
{

  public:
    // teardown after a failed start must not crash

    // No --host: the replica never becomes valid, so the mirror drops both.

    // A dialog that logs and is destroyed before the syslog thread delivers
    // its line: through the channel the line survives; connected straight to
    // the logger, as UI code did before step 6j, it is dropped.

    // SystemLogger::run() spends its first second in a processEvents loop on
    // the syslog thread; the destructor must still stop and join it promptly.

    // main() rebuilds the composition on every RESTART_CODE iteration.

  protected:
    void check_compositionRegistersAllLoggingProtocolsWithoutWindow();
    void check_servicesReferToTheCompositionsOwnObjects();
    void check_failedStartupBuildsNoServicesAndPerformsNoEcuIo();
    void check_workspaceOpensARomFromDisk();
    void check_migrationLoadsPreviousVersionSettingsFromDisk();
    void check_malformedSettingsRejectStartup();
    void check_settingsRewriteFailureIsAStartupWarning();
    void check_servicesShareTheCompositionsSession();
    void check_restartSeesSavedSettingsButNotTheDatalogDirectory();
    void check_waitRequestIsWiredToTheRemoteUtility();
    void check_remoteStateChangesReachThePeer();
    void check_mirroringWithoutAPeerReturnsPromptly();
    void check_channelLevelsReachTheLogWindowWithTheirPrefix();
    void check_debugLinesStayOutOfTheLogWindow();
    void check_relayedLineSurvivesItsSenderButADirectOneDoesNot();
    void check_enablingFileLoggingWritesASyslogFile();
    void check_destructionRightAfterConstructionDoesNotHang();
    void check_constructingTwiceInOneProcessSucceeds();
    void check_emptyHostSelectsTheDirectBackend();
    void check_nonEmptyHostSelectsTheRemoteBackendWithItsCredentials();
};

void DesktopCompositionTest::check_compositionRegistersAllLoggingProtocolsWithoutWindow()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    ASSERT_TRUE(composition.started());
    ASSERT_EQ(composition.services().logging_engine.registrations_.keys(), (QStringList{"CDBG", "MUT_DMA", "SSM"}));
}

TEST_F(DesktopCompositionTest, compositionRegistersAllLoggingProtocolsWithoutWindow)
{
    ASSERT_NO_FATAL_FAILURE(check_compositionRegistersAllLoggingProtocolsWithoutWindow());
}

void DesktopCompositionTest::check_servicesReferToTheCompositionsOwnObjects()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};

    const MainWindowServices first = composition.services();
    const MainWindowServices second = composition.services();

    ASSERT_EQ(&first.definition_catalogs, &second.definition_catalogs);
    ASSERT_EQ(&first.config_repository, &second.config_repository);
    ASSERT_EQ(&first.file_action_events, &second.file_action_events);
    ASSERT_EQ(&first.log, &second.log);
    ASSERT_EQ(&first.connection, &second.connection);
    ASSERT_EQ(&first.remote, &second.remote);
    ASSERT_EQ(&first.logging_engine, &second.logging_engine);
    ASSERT_EQ(&first.config, &second.config);
    ASSERT_EQ(&first.application, &second.application);
    ASSERT_EQ(&first.calibrations, composition.calibration_workspace_.get());
    ASSERT_EQ(&second.calibrations, &first.calibrations);
}

TEST_F(DesktopCompositionTest, servicesReferToTheCompositionsOwnObjects)
{
    ASSERT_NO_FATAL_FAILURE(check_servicesReferToTheCompositionsOwnObjects());
}

void DesktopCompositionTest::check_failedStartupBuildsNoServicesAndPerformsNoEcuIo()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString config_dir = root.path() + "/" + kVersion + "/config/";
    ASSERT_TRUE(QDir().mkpath(config_dir));
    ASSERT_TRUE(
        writeFile(config_dir + "protocols.cfg", R"(<config name="FastECU"><protocols/><car_models/></config>)"));

    DesktopComposition composition{{}, {}, root.path()};

    ASSERT_TRUE(!composition.started());
    ASSERT_TRUE(composition.startup_error().has_value());
    ASSERT_TRUE(QString::fromStdString(composition.startup_error()->detail).contains(config_dir + "protocols.cfg"));
    // Nothing that could log, thread, or talk to an ECU was created.
    ASSERT_TRUE(!composition.definition_catalogs_);
    ASSERT_TRUE(!composition.definition_service_);
    ASSERT_TRUE(!composition.syslog_thread_);
    ASSERT_TRUE(!composition.syslogger_);
    ASSERT_TRUE(!composition.serial_);
    ASSERT_TRUE(!composition.connection_);
    ASSERT_TRUE(!composition.remote_utility_);
    ASSERT_TRUE(!composition.logging_engine_);
    ASSERT_TRUE(!composition.rom_open_);
    ASSERT_TRUE(!composition.calibration_workspace_);
}

TEST_F(DesktopCompositionTest, failedStartupBuildsNoServicesAndPerformsNoEcuIo)
{
    ASSERT_NO_FATAL_FAILURE(check_failedStartupBuildsNoServicesAndPerformsNoEcuIo());
}

void DesktopCompositionTest::check_workspaceOpensARomFromDisk()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    ASSERT_TRUE(composition.started());
    ASSERT_TRUE(composition.calibration_workspace_);

    const QString rom_path = root.filePath("synthetic.bin");
    QFile rom{rom_path};
    ASSERT_TRUE(rom.open(QIODevice::WriteOnly));
    ASSERT_EQ(rom.write(QByteArray(2048, '\x5A')), qint64{2048});
    rom.close();

    const auto opened = composition.calibration_workspace_->open_file(rom_path.toStdString());

    ASSERT_TRUE(opened.has_value());
    const auto *session = composition.calibration_workspace_->find(opened->id);
    ASSERT_TRUE(session != nullptr);
    ASSERT_EQ(session->source().display_name, std::string("synthetic.bin"));
    ASSERT_EQ(session->protocol().file_size_label, std::string("2kb"));
    ASSERT_EQ(session->rom().size(), std::size_t{2048});
}

TEST_F(DesktopCompositionTest, workspaceOpensARomFromDisk)
{
    ASSERT_NO_FATAL_FAILURE(check_workspaceOpensARomFromDisk());
}

void DesktopCompositionTest::check_migrationLoadsPreviousVersionSettingsFromDisk()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString previous_dir = root.path() + "/0.1.0-beta.4/config/";
    ASSERT_TRUE(QDir().mkpath(previous_dir));
    ASSERT_TRUE(writeFile(previous_dir + "fastecu.cfg",
                          R"(<config name="FastECU"><software_settings>
<setting name="serial_port"><value data="ttyMIGRATED_UNIQUE"/></setting>
</software_settings></config>)"));
    const QString current_file = root.path() + "/" + kVersion + "/config/fastecu.cfg";
    ASSERT_TRUE(!QFile::exists(current_file));

    DesktopComposition composition{{}, {}, root.path()};

    ASSERT_TRUE(composition.started());
    ASSERT_EQ(composition.config_.settings().serial_port, std::string("ttyMIGRATED_UNIQUE"));
    QFile saved{current_file};
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    ASSERT_TRUE(saved.readAll().contains("ttyMIGRATED_UNIQUE"));
}

TEST_F(DesktopCompositionTest, migrationLoadsPreviousVersionSettingsFromDisk)
{
    ASSERT_NO_FATAL_FAILURE(check_migrationLoadsPreviousVersionSettingsFromDisk());
}

void DesktopCompositionTest::check_malformedSettingsRejectStartup()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString config_dir = root.path() + "/" + kVersion + "/config/";
    ASSERT_TRUE(QDir().mkpath(config_dir));
    ASSERT_TRUE(writeFile(config_dir + "fastecu.cfg", "<config"));

    DesktopComposition composition{{}, {}, root.path()};

    ASSERT_TRUE(!composition.started());
    const QString text = startup_failure_text(*composition.startup_error());
    ASSERT_TRUE(text.contains(config_dir + "fastecu.cfg"));
    ASSERT_TRUE(!composition.serial_);
}

TEST_F(DesktopCompositionTest, malformedSettingsRejectStartup)
{
    ASSERT_NO_FATAL_FAILURE(check_malformedSettingsRejectStartup());
}

void DesktopCompositionTest::check_settingsRewriteFailureIsAStartupWarning()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString config_dir = root.path() + "/" + kVersion + "/config/";
    ASSERT_TRUE(QDir().mkpath(config_dir));
    const QString config_file = config_dir + "fastecu.cfg";
    ASSERT_TRUE(writeFile(config_file, R"(<config name="FastECU" version="t"><software_settings/></config>)"));
    ASSERT_TRUE(QFile::setPermissions(config_file, QFileDevice::ReadOwner));
    const auto restore =
        qScopeGuard([&] { QFile::setPermissions(config_file, QFileDevice::ReadOwner | QFileDevice::WriteOwner); });
    if (QFile probe{config_file}; probe.open(QIODevice::WriteOnly | QIODevice::Append))
    {
        GTEST_SKIP() << "this filesystem ignores the read-only permission";
    }

    DesktopComposition composition{{}, {}, root.path()};

    ASSERT_TRUE(composition.started());
    ASSERT_TRUE(std::ranges::any_of(composition.startup_warnings(),
                                    [&](const QString& warning) { return warning.contains(config_file); }));
}

TEST_F(DesktopCompositionTest, settingsRewriteFailureIsAStartupWarning)
{
    ASSERT_NO_FATAL_FAILURE(check_settingsRewriteFailureIsAStartupWarning());
}

void DesktopCompositionTest::check_servicesShareTheCompositionsSession()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    ASSERT_TRUE(composition.started());
    ASSERT_EQ(&composition.services().config, &composition.config_);
    ASSERT_EQ(composition.services().application.version, std::string(kVersion));
    ASSERT_EQ(QString::fromStdString(composition.config_.provisioned_paths().base_config_directory), root.path());
}

TEST_F(DesktopCompositionTest, servicesShareTheCompositionsSession)
{
    ASSERT_NO_FATAL_FAILURE(check_servicesShareTheCompositionsSession());
}

void DesktopCompositionTest::check_restartSeesSavedSettingsButNotTheDatalogDirectory()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    std::string provisioned_datalogs;
    {
        DesktopComposition first{{}, {}, root.path()};
        ASSERT_TRUE(first.started());
        provisioned_datalogs = first.config_.provisioned_paths().datalog_files_directory;
        first.config_.settings().serial_port = "ttyRESTART";
        first.config_.settings().datalog_files_directory = root.path().toStdString() + "/elsewhere/";
        ASSERT_TRUE(first.config_.save().has_value());
    }
    DesktopComposition second{{}, {}, root.path()};
    ASSERT_TRUE(second.started());
    ASSERT_EQ(second.config_.settings().serial_port, std::string("ttyRESTART"));
    ASSERT_EQ(second.config_.settings().datalog_files_directory, provisioned_datalogs);
}

TEST_F(DesktopCompositionTest, restartSeesSavedSettingsButNotTheDatalogDirectory)
{
    ASSERT_NO_FATAL_FAILURE(check_restartSeesSavedSettingsButNotTheDatalogDirectory());
}

void DesktopCompositionTest::check_waitRequestIsWiredToTheRemoteUtility()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    // Running the wait needs a peer; it loops until one answers, so this
    // checks the wiring without invoking it: isSignalConnected is
    // protected, so disconnect-and-report-whether-anything-was-there
    // is the public way to observe the same fact.
    ASSERT_TRUE(QObject::disconnect(&composition.services().remote, &RemotePeer::wait_requested, nullptr, nullptr));
}

TEST_F(DesktopCompositionTest, waitRequestIsWiredToTheRemoteUtility)
{
    ASSERT_NO_FATAL_FAILURE(check_waitRequestIsWiredToTheRemoteUtility());
}

void DesktopCompositionTest::check_remoteStateChangesReachThePeer()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    fastecu::testing::SignalRecorder changes{&composition.services().remote, &RemotePeer::stateChanged};

    emit composition.remote_utility_->stateChanged(QRemoteObjectReplica::Suspect, QRemoteObjectReplica::Valid);

    ASSERT_EQ(changes.count(), 1);
    ASSERT_EQ(std::get<0>(changes.snapshot().at(0)), QRemoteObjectReplica::Suspect);
    ASSERT_EQ(std::get<1>(changes.snapshot().at(0)), QRemoteObjectReplica::Valid);
}

TEST_F(DesktopCompositionTest, remoteStateChangesReachThePeer)
{
    ASSERT_NO_FATAL_FAILURE(check_remoteStateChangesReachThePeer());
}

void DesktopCompositionTest::check_mirroringWithoutAPeerReturnsPromptly()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    RemotePeer& remote = composition.services().remote;
    ASSERT_TRUE(!composition.remote_utility_->isValid());

    QElapsedTimer elapsed;
    elapsed.start();
    emit remote.log_window_message("mirrored line");
    emit remote.progress(42);
    ASSERT_TRUE(elapsed.elapsed() < 1000) << "mirroring without a peer blocked";
}

TEST_F(DesktopCompositionTest, mirroringWithoutAPeerReturnsPromptly)
{
    ASSERT_NO_FATAL_FAILURE(check_mirroringWithoutAPeerReturnsPromptly());
}

void DesktopCompositionTest::check_channelLevelsReachTheLogWindowWithTheirPrefix()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    LogChannel& log = composition.services().log;
    fastecu::testing::SignalRecorder window{&log, &LogChannel::log_window_message};

    emit log.LOG_E("error line", true, false);
    emit log.LOG_W("warning line", true, false);
    emit log.LOG_I("info line", true, false);

    // Match by content: the logger's own "SystemLogger started..." line
    // can reach the window too.
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return has_line_ending_with(window, "(II) info line"); },
                                             std::chrono::milliseconds(5000)));
    ASSERT_TRUE(has_line_ending_with(window, "(EE) error line"));
    ASSERT_TRUE(has_line_ending_with(window, "(WW) warning line"));
}

TEST_F(DesktopCompositionTest, channelLevelsReachTheLogWindowWithTheirPrefix)
{
    ASSERT_NO_FATAL_FAILURE(check_channelLevelsReachTheLogWindowWithTheirPrefix());
}

void DesktopCompositionTest::check_debugLinesStayOutOfTheLogWindow()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    LogChannel& log = composition.services().log;
    fastecu::testing::SignalRecorder window{&log, &LogChannel::log_window_message};

    emit log.LOG_D("debug line", true, false);
    emit log.LOG_I("sentinel", false, false);

    ASSERT_TRUE(fastecu::testing::wait_until([&] { return has_line_ending_with(window, "sentinel"); },
                                             std::chrono::milliseconds(5000)));
    ASSERT_TRUE(!has_line_containing(window, "debug line"));
}

TEST_F(DesktopCompositionTest, debugLinesStayOutOfTheLogWindow)
{
    ASSERT_NO_FATAL_FAILURE(check_debugLinesStayOutOfTheLogWindow());
}

void DesktopCompositionTest::check_relayedLineSurvivesItsSenderButADirectOneDoesNot()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    LogChannel& log = composition.services().log;
    fastecu::testing::SignalRecorder window{&log, &LogChannel::log_window_message};

    {
        SyslogGate gate{*composition.syslogger_};
        auto relayed = std::make_unique<LogChannel>();
        QObject::connect(relayed.get(), &LogChannel::LOG_I, &log, &LogChannel::LOG_I);
        auto direct = std::make_unique<LogChannel>();
        QObject::connect(direct.get(), &LogChannel::LOG_I, composition.syslogger_.get(), &SystemLogger::log_messages);

        emit relayed->LOG_I("relayed line", false, false);
        emit direct->LOG_I("direct line", false, false);
        relayed.reset();
        direct.reset();
    }
    emit log.LOG_I("sentinel", false, false);

    ASSERT_TRUE(fastecu::testing::wait_until([&] { return has_line_ending_with(window, "sentinel"); },
                                             std::chrono::milliseconds(5000)));
    ASSERT_TRUE(has_line_ending_with(window, "relayed line"));
    ASSERT_TRUE(!has_line_containing(window, "direct line"));
}

TEST_F(DesktopCompositionTest, relayedLineSurvivesItsSenderButADirectOneDoesNot)
{
    ASSERT_NO_FATAL_FAILURE(check_relayedLineSurvivesItsSenderButADirectOneDoesNot());
}

void DesktopCompositionTest::check_enablingFileLoggingWritesASyslogFile()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    const QString syslog_dir =
        QString::fromStdString(composition.services().config.effective_paths().syslog_files_directory);
    ASSERT_TRUE(!syslog_dir.isEmpty());
    ASSERT_TRUE(QDir().mkpath(syslog_dir));
    LogChannel& log = composition.services().log;
    fastecu::testing::SignalRecorder window{&log, &LogChannel::log_window_message};

    emit log.enable_log_write_to_file(true);
    emit log.LOG_I("to file", false, true);
    emit log.LOG_D("debug to file", false, true);
    // log_messages signals the window before it writes the file; the
    // sentinel's window line proves the earlier write has finished.
    emit log.LOG_I("sentinel", false, false);

    ASSERT_TRUE(fastecu::testing::wait_until([&] { return has_line_ending_with(window, "sentinel"); },
                                             std::chrono::milliseconds(5000)));
    const QStringList files = QDir(syslog_dir).entryList({"log_fastecu_*.txt"}, QDir::Files);
    ASSERT_EQ(files.size(), 1);
    QFile file{QDir(syslog_dir).filePath(files.first())};
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    ASSERT_TRUE(contents.contains("to file"));
    ASSERT_TRUE(contents.contains("debug to file"));
}

TEST_F(DesktopCompositionTest, enablingFileLoggingWritesASyslogFile)
{
    ASSERT_NO_FATAL_FAILURE(check_enablingFileLoggingWritesASyslogFile());
}

void DesktopCompositionTest::check_destructionRightAfterConstructionDoesNotHang()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    QElapsedTimer elapsed;
    elapsed.start();
    {
        DesktopComposition composition{{}, {}, root.path()};
    }
    ASSERT_TRUE(elapsed.elapsed() < 5000) << "composition teardown took longer than 5 s";
}

TEST_F(DesktopCompositionTest, destructionRightAfterConstructionDoesNotHang)
{
    ASSERT_NO_FATAL_FAILURE(check_destructionRightAfterConstructionDoesNotHang());
}

void DesktopCompositionTest::check_constructingTwiceInOneProcessSucceeds()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    for (int iteration = 0; iteration < 2; ++iteration)
    {
        DesktopComposition composition{{}, {}, root.path()};
        ASSERT_TRUE(composition.started());
        ASSERT_EQ(QString::fromStdString(composition.services().config.provisioned_paths().base_config_directory),
                  root.path());
    }
}

TEST_F(DesktopCompositionTest, constructingTwiceInOneProcessSucceeds)
{
    ASSERT_NO_FATAL_FAILURE(check_constructingTwiceInOneProcessSucceeds());
}

void DesktopCompositionTest::check_emptyHostSelectsTheDirectBackend()
{
    ASSERT_TRUE(std::holds_alternative<DirectSerial>(serial_connection_from_args({}, {})));
    ASSERT_TRUE(std::holds_alternative<DirectSerial>(serial_connection_from_args({}, "ignored")));
}

TEST_F(DesktopCompositionTest, emptyHostSelectsTheDirectBackend)
{
    ASSERT_NO_FATAL_FAILURE(check_emptyHostSelectsTheDirectBackend());
}

void DesktopCompositionTest::check_nonEmptyHostSelectsTheRemoteBackendWithItsCredentials()
{
    const SerialConnection connection = serial_connection_from_args("peer.example:1234", "secret");
    const auto *remote = std::get_if<RemoteSerial>(&connection);
    ASSERT_TRUE(remote != nullptr);
    ASSERT_EQ(remote->address, QString("peer.example:1234"));
    ASSERT_EQ(remote->password, QString("secret"));
}

TEST_F(DesktopCompositionTest, nonEmptyHostSelectsTheRemoteBackendWithItsCredentials)
{
    ASSERT_NO_FATAL_FAILURE(check_nonEmptyHostSelectsTheRemoteBackendWithItsCredentials());
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
