#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <variant>
#include <QMap>
#include "src/platform/desktop/common/logging/runtime/logging_worker.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#define private public
#include "src/platform/desktop/common/logging/runtime/logging_engine.h"
#undef private

#include "apps/desktop/desktop_composition.h"
#include "apps/desktop/startup_diagnostics.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/definition/definition_catalog_session.h"

#include <QDir>
#include <QFile>
#include <QSemaphore>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"

#include <algorithm>
#include <memory>

#include "src/platform/desktop/common/logging/runtime/systemlogger.h"
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

// DesktopComposition befriends this fixture only. TEST_F bodies are members of
// generated subclasses, so they reach its private state through these accessors.
class DesktopCompositionTest : public ::testing::Test
{
  protected:
    static auto& config_of(DesktopComposition& composition)
    {
        return composition.config_;
    }
    static auto& calibration_workspace_of(DesktopComposition& composition)
    {
        return composition.calibration_workspace_;
    }
    static auto& definition_catalogs_of(DesktopComposition& composition)
    {
        return composition.definition_catalogs_;
    }
    static auto& definition_service_of(DesktopComposition& composition)
    {
        return composition.definition_service_;
    }
    static auto& syslog_thread_of(DesktopComposition& composition)
    {
        return composition.syslog_thread_;
    }
    static auto& syslogger_of(DesktopComposition& composition)
    {
        return composition.syslogger_;
    }
    static auto& serial_of(DesktopComposition& composition)
    {
        return composition.serial_;
    }
    static auto& connection_of(DesktopComposition& composition)
    {
        return composition.connection_;
    }
    static auto& remote_utility_of(DesktopComposition& composition)
    {
        return composition.remote_utility_;
    }
    static auto& logging_engine_of(DesktopComposition& composition)
    {
        return composition.logging_engine_;
    }
    static auto& rom_open_of(DesktopComposition& composition)
    {
        return composition.rom_open_;
    }
};

TEST_F(DesktopCompositionTest, compositionRegistersAllLoggingProtocolsWithoutWindow)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    ASSERT_TRUE(composition.started());
    ASSERT_EQ(composition.services().logging_engine.registrations_.keys(), (QStringList{"CDBG", "MUT_DMA", "SSM"}));
}

TEST_F(DesktopCompositionTest, servicesReferToTheCompositionsOwnObjects)
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
    ASSERT_EQ(&first.calibrations, calibration_workspace_of(composition).get());
    ASSERT_EQ(&second.calibrations, &first.calibrations);
}

// teardown after a failed start must not crash
TEST_F(DesktopCompositionTest, failedStartupBuildsNoServicesAndPerformsNoEcuIo)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString config_dir = root.path() + "/" + kVersion + "/config/";
    ASSERT_TRUE(QDir().mkpath(config_dir));
    ASSERT_TRUE(writeFile(config_dir + "fastecu.cfg", "<config"));

    DesktopComposition composition{{}, {}, root.path()};

    ASSERT_TRUE(!composition.started());
    ASSERT_TRUE(composition.startup_error().has_value());
    ASSERT_TRUE(QString::fromStdString(composition.startup_error()->detail).contains(config_dir + "fastecu.cfg"));
    // Nothing that could log, thread, or talk to an ECU was created.
    ASSERT_TRUE(!definition_catalogs_of(composition));
    ASSERT_TRUE(!definition_service_of(composition));
    ASSERT_TRUE(!syslog_thread_of(composition));
    ASSERT_TRUE(!syslogger_of(composition));
    ASSERT_TRUE(!serial_of(composition));
    ASSERT_TRUE(!connection_of(composition));
    ASSERT_TRUE(!remote_utility_of(composition));
    ASSERT_TRUE(!logging_engine_of(composition));
    ASSERT_TRUE(!rom_open_of(composition));
    ASSERT_TRUE(!calibration_workspace_of(composition));
}

TEST_F(DesktopCompositionTest, workspaceOpensARomFromDisk)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    ASSERT_TRUE(composition.started());
    ASSERT_TRUE(calibration_workspace_of(composition));

    const QString rom_path = root.filePath("synthetic.bin");
    QFile rom{rom_path};
    ASSERT_TRUE(rom.open(QIODevice::WriteOnly));
    ASSERT_EQ(rom.write(QByteArray(2048, '\x5A')), qint64{2048});
    rom.close();

    const auto opened = calibration_workspace_of(composition)->open_file(rom_path.toStdString());

    ASSERT_TRUE(opened.has_value());
    const auto *session = calibration_workspace_of(composition)->find(opened->id);
    ASSERT_TRUE(session != nullptr);
    ASSERT_EQ(session->source().display_name, std::string("synthetic.bin"));
    ASSERT_EQ(session->protocol().file_size_label, std::string("2kb"));
    ASSERT_EQ(session->rom().size(), std::size_t{2048});
}

TEST_F(DesktopCompositionTest, migrationLoadsPreviousVersionSettingsFromDisk)
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
    ASSERT_EQ(config_of(composition).settings().serial_port, std::string("ttyMIGRATED_UNIQUE"));
    QFile saved{current_file};
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    ASSERT_TRUE(saved.readAll().contains("ttyMIGRATED_UNIQUE"));
}

TEST_F(DesktopCompositionTest, aPreviousVersionsSavedRowSelectsNoVehicle)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString previous_dir = root.path() + "/0.1.0-beta.4/config/";
    ASSERT_TRUE(QDir().mkpath(previous_dir));
    ASSERT_TRUE(writeFile(previous_dir + "fastecu.cfg", R"(<config name="FastECU"><software_settings>
<setting name="protocol_id"><value data="35"/></setting>
</software_settings></config>)"));

    DesktopComposition composition{{}, {}, root.path()};

    ASSERT_TRUE(composition.started());
    ASSERT_TRUE(config_of(composition).selected_vehicle() == nullptr);
}

TEST_F(DesktopCompositionTest, malformedSettingsRejectStartup)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString config_dir = root.path() + "/" + kVersion + "/config/";
    ASSERT_TRUE(QDir().mkpath(config_dir));
    ASSERT_TRUE(writeFile(config_dir + "fastecu.cfg", "<config"));

    DesktopComposition composition{{}, {}, root.path()};

    ASSERT_TRUE(!composition.started());
    const auto& startup_error = composition.startup_error();
    ASSERT_TRUE(startup_error.has_value());
    const QString text = startup_failure_text(*startup_error);
    ASSERT_TRUE(text.contains(config_dir + "fastecu.cfg"));
    ASSERT_TRUE(!serial_of(composition));
}

TEST_F(DesktopCompositionTest, settingsRewriteFailureIsAStartupWarning)
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

TEST_F(DesktopCompositionTest, servicesShareTheCompositionsSession)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    ASSERT_TRUE(composition.started());
    ASSERT_EQ(&composition.services().config, &config_of(composition));
    ASSERT_EQ(composition.services().application.version, std::string(kVersion));
    ASSERT_EQ(QString::fromStdString(config_of(composition).provisioned_paths().base_config_directory), root.path());
}

TEST_F(DesktopCompositionTest, restartSeesSavedSettingsAndTheDatalogDirectory)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const std::string datalogs = root.path().toStdString() + "/elsewhere/";
    {
        DesktopComposition first{{}, {}, root.path()};
        ASSERT_TRUE(first.started());
        config_of(first).settings().serial_port = "ttyRESTART";
        config_of(first).settings().datalog_files_directory = datalogs;
        ASSERT_TRUE(config_of(first).save().has_value());
    }
    DesktopComposition second{{}, {}, root.path()};
    ASSERT_TRUE(second.started());
    ASSERT_EQ(config_of(second).settings().serial_port, std::string("ttyRESTART"));
    ASSERT_EQ(config_of(second).settings().datalog_files_directory, datalogs);
}

TEST_F(DesktopCompositionTest, waitRequestIsWiredToTheRemoteUtility)
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

TEST_F(DesktopCompositionTest, remoteStateChangesReachThePeer)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    fastecu::testing::SignalRecorder changes{&composition.services().remote, &RemotePeer::stateChanged};

    emit remote_utility_of(composition)->stateChanged(QRemoteObjectReplica::Suspect, QRemoteObjectReplica::Valid);

    ASSERT_EQ(changes.count(), 1U);
    ASSERT_EQ(std::get<0>(changes.snapshot().at(0)), QRemoteObjectReplica::Suspect);
    ASSERT_EQ(std::get<1>(changes.snapshot().at(0)), QRemoteObjectReplica::Valid);
}

// No --host: the replica never becomes valid, so the mirror drops both.
TEST_F(DesktopCompositionTest, mirroringWithoutAPeerReturnsPromptly)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    RemotePeer& remote = composition.services().remote;
    ASSERT_TRUE(!remote_utility_of(composition)->isValid());

    QElapsedTimer elapsed;
    elapsed.start();
    emit remote.log_window_message("mirrored line");
    emit remote.progress(42);
    ASSERT_TRUE(elapsed.elapsed() < 1000) << "mirroring without a peer blocked";
}

TEST_F(DesktopCompositionTest, channelLevelsReachTheLogWindowWithTheirPrefix)
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

TEST_F(DesktopCompositionTest, debugLinesStayOutOfTheLogWindow)
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

// A dialog that logs and is destroyed before the syslog thread delivers
// its line: through the channel the line survives; connected straight to
// the logger, as UI code did before step 6j, it is dropped.
TEST_F(DesktopCompositionTest, relayedLineSurvivesItsSenderButADirectOneDoesNot)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{{}, {}, root.path()};
    LogChannel& log = composition.services().log;
    fastecu::testing::SignalRecorder window{&log, &LogChannel::log_window_message};

    {
        SyslogGate gate{*syslogger_of(composition)};
        auto relayed = std::make_unique<LogChannel>();
        QObject::connect(relayed.get(), &LogChannel::LOG_I, &log, &LogChannel::LOG_I);
        auto direct = std::make_unique<LogChannel>();
        QObject::connect(direct.get(), &LogChannel::LOG_I, syslogger_of(composition).get(),
                         &SystemLogger::log_messages);

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

TEST_F(DesktopCompositionTest, enablingFileLoggingWritesASyslogFile)
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
    ASSERT_EQ(files.size(), 1U);
    QFile file{QDir(syslog_dir).filePath(files.first())};
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    ASSERT_TRUE(contents.contains("to file"));
    ASSERT_TRUE(contents.contains("debug to file"));
}

// The destructor must stop and join the syslog thread promptly.
TEST_F(DesktopCompositionTest, destructionRightAfterConstructionDoesNotHang)
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

// main() rebuilds the composition on every RESTART_CODE iteration.
TEST_F(DesktopCompositionTest, constructingTwiceInOneProcessSucceeds)
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

TEST_F(DesktopCompositionTest, emptyHostSelectsTheDirectBackend)
{
    ASSERT_TRUE(std::holds_alternative<DirectSerial>(serial_connection_from_args({}, {})));
    ASSERT_TRUE(std::holds_alternative<DirectSerial>(serial_connection_from_args({}, "ignored")));
}

TEST_F(DesktopCompositionTest, nonEmptyHostSelectsTheRemoteBackendWithItsCredentials)
{
    const SerialConnection connection = serial_connection_from_args("peer.example:1234", "secret");
    const auto *remote = std::get_if<RemoteSerial>(&connection);
    ASSERT_TRUE(remote != nullptr);
    ASSERT_EQ(remote->address, QString("peer.example:1234"));
    ASSERT_EQ(remote->password, QString("secret"));
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
