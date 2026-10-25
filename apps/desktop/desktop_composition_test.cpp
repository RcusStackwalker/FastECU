#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <tuple>
#include <QMap>
#include "src/backend/ports/testing/result_matchers.h"
#include "src/platform/desktop/common/logging/runtime/logging_worker.h"
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
#include "src/ui/desktop/channels/log_channel.h"

namespace
{

using fastecu::testing::IsOk;
using fastecu::ui::LogChannel;
using ::testing::_;
using ::testing::Optional;

constexpr auto kVersion = "0.1.0-beta.5";

bool WriteFile(const QString& path, const QString& text)
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
        Release();
    }
    SyslogGate(const SyslogGate&) = delete;
    SyslogGate& operator=(const SyslogGate&) = delete;

    void Release()
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

bool HasLineEndingWith(const auto& spy, const QString& suffix)
{
    return std::ranges::any_of(spy.Snapshot(),
                               [&](const auto& arguments) { return std::get<0>(arguments).endsWith(suffix); });
}

bool HasLineContaining(const auto& spy, const QString& text)
{
    return std::ranges::any_of(spy.Snapshot(),
                               [&](const auto& arguments) { return std::get<0>(arguments).contains(text); });
}

} // namespace

// DesktopComposition befriends this fixture only. TEST_F bodies are members of
// generated subclasses, so they reach its private state through these accessors.
class DesktopCompositionTest : public ::testing::Test
{
  protected:
    static auto& ConfigOf(DesktopComposition& composition)
    {
        return composition.config_;
    }
    static auto& CalibrationWorkspaceOf(DesktopComposition& composition)
    {
        return composition.calibration_workspace_;
    }
    static auto& DefinitionCatalogsOf(DesktopComposition& composition)
    {
        return composition.definition_catalogs_;
    }
    static auto& DefinitionServiceOf(DesktopComposition& composition)
    {
        return composition.definition_service_;
    }
    static auto& SyslogThreadOf(DesktopComposition& composition)
    {
        return composition.syslog_thread_;
    }
    static auto& SysloggerOf(DesktopComposition& composition)
    {
        return composition.syslogger_;
    }
    static auto& SerialOf(DesktopComposition& composition)
    {
        return composition.serial_;
    }
    static auto& ConnectionOf(DesktopComposition& composition)
    {
        return composition.connection_;
    }
    static auto& LoggingEngineOf(DesktopComposition& composition)
    {
        return composition.logging_engine_;
    }
    static auto& RomOpenOf(DesktopComposition& composition)
    {
        return composition.rom_open_;
    }
};

TEST_F(DesktopCompositionTest, compositionRegistersAllLoggingProtocolsWithoutWindow)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{root.path()};
    ASSERT_TRUE(composition.Started());
    ASSERT_EQ(composition.Services().logging_engine.registrations_.keys(), (QStringList{"CDBG", "MUT_DMA", "SSM"}));
}

TEST_F(DesktopCompositionTest, servicesReferToTheCompositionsOwnObjects)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{root.path()};

    const MainWindowServices first = composition.Services();
    const MainWindowServices second = composition.Services();

    ASSERT_EQ(&first.definition_catalogs, &second.definition_catalogs);
    ASSERT_EQ(&first.config_repository, &second.config_repository);
    ASSERT_EQ(&first.file_action_events, &second.file_action_events);
    ASSERT_EQ(&first.log, &second.log);
    ASSERT_EQ(&first.connection, &second.connection);
    ASSERT_EQ(&first.logging_engine, &second.logging_engine);
    ASSERT_EQ(&first.config, &second.config);
    ASSERT_EQ(&first.application, &second.application);
    ASSERT_EQ(&first.calibrations, CalibrationWorkspaceOf(composition).get());
    ASSERT_EQ(&second.calibrations, &first.calibrations);
}

// teardown after a failed start must not crash
TEST_F(DesktopCompositionTest, failedStartupBuildsNoServicesAndPerformsNoEcuIo)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString config_dir = root.path() + "/" + kVersion + "/config/";
    ASSERT_TRUE(QDir().mkpath(config_dir));
    ASSERT_TRUE(WriteFile(config_dir + "fastecu.cfg", "<config"));

    DesktopComposition composition{root.path()};

    ASSERT_TRUE(!composition.Started());
    ASSERT_THAT(composition.StartupError(), Optional(_));
    ASSERT_TRUE(QString::fromStdString(composition.StartupError()->detail).contains(config_dir + "fastecu.cfg"));
    // Nothing that could log, thread, or talk to an ECU was created.
    ASSERT_TRUE(!DefinitionCatalogsOf(composition));
    ASSERT_TRUE(!DefinitionServiceOf(composition));
    ASSERT_TRUE(!SyslogThreadOf(composition));
    ASSERT_TRUE(!SysloggerOf(composition));
    ASSERT_TRUE(!SerialOf(composition));
    ASSERT_TRUE(!ConnectionOf(composition));
    ASSERT_TRUE(!LoggingEngineOf(composition));
    ASSERT_TRUE(!RomOpenOf(composition));
    ASSERT_TRUE(!CalibrationWorkspaceOf(composition));
}

TEST_F(DesktopCompositionTest, workspaceOpensARomFromDisk)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{root.path()};
    ASSERT_TRUE(composition.Started());
    ASSERT_TRUE(CalibrationWorkspaceOf(composition));

    const QString rom_path = root.filePath("synthetic.bin");
    QFile rom{rom_path};
    ASSERT_TRUE(rom.open(QIODevice::WriteOnly));
    ASSERT_EQ(rom.write(QByteArray(2048, '\x5A')), qint64{2048});
    rom.close();

    const auto opened = CalibrationWorkspaceOf(composition)->OpenFile(rom_path.toStdString());

    ASSERT_THAT(opened, IsOk());
    const auto *session = CalibrationWorkspaceOf(composition)->Find(opened->id);
    ASSERT_TRUE(session != nullptr);
    ASSERT_EQ(session->Source().display_name, std::string("synthetic.bin"));
    ASSERT_EQ(session->Protocol().file_size_label, std::string("2kb"));
    ASSERT_EQ(session->File().size(), std::size_t{2048});
}

TEST_F(DesktopCompositionTest, migrationLoadsPreviousVersionSettingsFromDisk)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString previous_dir = root.path() + "/0.1.0-beta.4/config/";
    ASSERT_TRUE(QDir().mkpath(previous_dir));
    ASSERT_TRUE(WriteFile(previous_dir + "fastecu.cfg",
                          R"(<config name="FastECU"><software_settings>
<setting name="serial_port"><value data="ttyMIGRATED_UNIQUE"/></setting>
</software_settings></config>)"));
    const QString current_file = root.path() + "/" + kVersion + "/config/fastecu.cfg";
    ASSERT_TRUE(!QFile::exists(current_file));

    DesktopComposition composition{root.path()};

    ASSERT_TRUE(composition.Started());
    ASSERT_EQ(ConfigOf(composition).Settings().serial_port, std::string("ttyMIGRATED_UNIQUE"));
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
    ASSERT_TRUE(WriteFile(previous_dir + "fastecu.cfg", R"(<config name="FastECU"><software_settings>
<setting name="protocol_id"><value data="35"/></setting>
</software_settings></config>)"));

    DesktopComposition composition{root.path()};

    ASSERT_TRUE(composition.Started());
    ASSERT_TRUE(ConfigOf(composition).SelectedVehicle() == nullptr);
}

TEST_F(DesktopCompositionTest, malformedSettingsRejectStartup)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString config_dir = root.path() + "/" + kVersion + "/config/";
    ASSERT_TRUE(QDir().mkpath(config_dir));
    ASSERT_TRUE(WriteFile(config_dir + "fastecu.cfg", "<config"));

    DesktopComposition composition{root.path()};

    ASSERT_TRUE(!composition.Started());
    const auto& startup_error = composition.StartupError();
    ASSERT_THAT(startup_error, Optional(_));
    const QString text = StartupFailureText(*startup_error);
    ASSERT_TRUE(text.contains(config_dir + "fastecu.cfg"));
    ASSERT_TRUE(!SerialOf(composition));
}

TEST_F(DesktopCompositionTest, settingsRewriteFailureIsAStartupWarning)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString config_dir = root.path() + "/" + kVersion + "/config/";
    ASSERT_TRUE(QDir().mkpath(config_dir));
    const QString config_file = config_dir + "fastecu.cfg";
    ASSERT_TRUE(WriteFile(config_file, R"(<config name="FastECU" version="t"><software_settings/></config>)"));
    ASSERT_TRUE(QFile::setPermissions(config_file, QFileDevice::ReadOwner));
    const auto restore =
        qScopeGuard([&] { QFile::setPermissions(config_file, QFileDevice::ReadOwner | QFileDevice::WriteOwner); });
    if (QFile probe{config_file}; probe.open(QIODevice::WriteOnly | QIODevice::Append))
    {
        GTEST_SKIP() << "this filesystem ignores the read-only permission";
    }

    DesktopComposition composition{root.path()};

    ASSERT_TRUE(composition.Started());
    // Joined so a failure prints the warnings as text; a path has no '\n',
    // so no match can span two warnings.
    ASSERT_THAT(composition.StartupWarnings().join('\n').toStdString(),
                ::testing::HasSubstr(config_file.toStdString()));
}

TEST_F(DesktopCompositionTest, servicesShareTheCompositionsSession)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{root.path()};
    ASSERT_TRUE(composition.Started());
    ASSERT_EQ(&composition.Services().config, &ConfigOf(composition));
    ASSERT_EQ(composition.Services().application.version, std::string(kVersion));
    ASSERT_EQ(QString::fromStdString(ConfigOf(composition).ProvisionedPaths().base_config_directory), root.path());
}

TEST_F(DesktopCompositionTest, restartSeesSavedSettingsAndTheDatalogDirectory)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const std::string datalogs = root.path().toStdString() + "/elsewhere/";
    {
        DesktopComposition first{root.path()};
        ASSERT_TRUE(first.Started());
        ConfigOf(first).Settings().serial_port = "ttyRESTART";
        ConfigOf(first).Settings().datalog_files_directory = datalogs;
        ASSERT_THAT(ConfigOf(first).Save(), IsOk());
    }
    DesktopComposition second{root.path()};
    ASSERT_TRUE(second.Started());
    ASSERT_EQ(ConfigOf(second).Settings().serial_port, std::string("ttyRESTART"));
    ASSERT_EQ(ConfigOf(second).Settings().datalog_files_directory, datalogs);
}

TEST_F(DesktopCompositionTest, channelLevelsReachTheLogWindowWithTheirPrefix)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{root.path()};
    LogChannel& log = composition.Services().log;
    fastecu::testing::SignalRecorder window{&log, &LogChannel::logWindowMessage};

    emit log.logE("error line", true, false);
    emit log.logW("warning line", true, false);
    emit log.logI("info line", true, false);

    // Match by content: the logger's own "SystemLogger started..." line
    // can reach the window too.
    ASSERT_TRUE(fastecu::testing::WaitUntil([&] { return HasLineEndingWith(window, "(II) info line"); },
                                            std::chrono::milliseconds(5000)));
    ASSERT_TRUE(HasLineEndingWith(window, "(EE) error line"));
    ASSERT_TRUE(HasLineEndingWith(window, "(WW) warning line"));
}

TEST_F(DesktopCompositionTest, debugLinesStayOutOfTheLogWindow)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{root.path()};
    LogChannel& log = composition.Services().log;
    fastecu::testing::SignalRecorder window{&log, &LogChannel::logWindowMessage};

    emit log.logD("debug line", true, false);
    emit log.logI("sentinel", false, false);

    ASSERT_TRUE(fastecu::testing::WaitUntil([&] { return HasLineEndingWith(window, "sentinel"); },
                                            std::chrono::milliseconds(5000)));
    ASSERT_TRUE(!HasLineContaining(window, "debug line"));
}

// A dialog that logs and is destroyed before the syslog thread delivers
// its line: through the channel the line survives; connected straight to
// the logger, as UI code did before step 6j, it is dropped.
TEST_F(DesktopCompositionTest, relayedLineSurvivesItsSenderButADirectOneDoesNot)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{root.path()};
    LogChannel& log = composition.Services().log;
    fastecu::testing::SignalRecorder window{&log, &LogChannel::logWindowMessage};

    {
        SyslogGate gate{*SysloggerOf(composition)};
        auto relayed = std::make_unique<LogChannel>();
        QObject::connect(relayed.get(), &LogChannel::logI, &log, &LogChannel::logI);
        auto direct = std::make_unique<LogChannel>();
        QObject::connect(direct.get(), &LogChannel::logI, SysloggerOf(composition).get(), &SystemLogger::logMessages);

        emit relayed->logI("relayed line", false, false);
        emit direct->logI("direct line", false, false);
        relayed.reset();
        direct.reset();
    }
    emit log.logI("sentinel", false, false);

    ASSERT_TRUE(fastecu::testing::WaitUntil([&] { return HasLineEndingWith(window, "sentinel"); },
                                            std::chrono::milliseconds(5000)));
    ASSERT_TRUE(HasLineEndingWith(window, "relayed line"));
    ASSERT_TRUE(!HasLineContaining(window, "direct line"));
}

TEST_F(DesktopCompositionTest, enablingFileLoggingWritesASyslogFile)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    DesktopComposition composition{root.path()};
    const QString syslog_dir =
        QString::fromStdString(composition.Services().config.EffectivePaths().syslog_files_directory);
    ASSERT_TRUE(!syslog_dir.isEmpty());
    ASSERT_TRUE(QDir().mkpath(syslog_dir));
    LogChannel& log = composition.Services().log;
    fastecu::testing::SignalRecorder window{&log, &LogChannel::logWindowMessage};

    emit log.enableLogWriteToFile(true);
    emit log.logI("to file", false, true);
    emit log.logD("debug to file", false, true);
    // log_messages signals the window before it writes the file; the
    // sentinel's window line proves the earlier write has finished.
    emit log.logI("sentinel", false, false);

    ASSERT_TRUE(fastecu::testing::WaitUntil([&] { return HasLineEndingWith(window, "sentinel"); },
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
        DesktopComposition composition{root.path()};
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
        SCOPED_TRACE(::testing::PrintToString(iteration));
        DesktopComposition composition{root.path()};
        ASSERT_TRUE(composition.Started());
        ASSERT_EQ(QString::fromStdString(composition.Services().config.ProvisionedPaths().base_config_directory),
                  root.path());
    }
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
