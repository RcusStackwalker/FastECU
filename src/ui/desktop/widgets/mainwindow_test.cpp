#include <cctype>
#include <string>
#include <vector>
#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QScopeGuard>
#include <QSet>
#include <QSettings>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMessageBox>
#include <QRadioButton>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QWidgetAction>
#include <gtest/gtest.h>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <QSemaphore>
#include <QTimer>
#include <QTreeWidget>

#include <gmock/gmock.h>
#include "src/backend/logging/testing/scripted_logging_protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include "src/ui/desktop/widgets/mainwindow.h"
#include "src/platform/desktop/common/definition/definition_catalog_session.h"
#include "src/backend/logging/logger_definition_service.h"
#include "ui_mainwindow.h"

#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/connection/testing/adapter_connection_harness.h"
#include "src/ui/desktop/connection/connection_coordinator.h"
#include "src/ui/desktop/widgets/qt_identify_launcher.h"
#include "src/platform/desktop/common/logging/runtime/logging_engine.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/event_sink/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/ui/desktop/channels/log_channel.h"
#include "src/ui/desktop/channels/remote_peer.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/rom_open.h"
#include "src/backend/definition/definition_service.h"
#include "src/ui/desktop/calibration/session_key.h"
#include "src/backend/flash/flash_operation_request.h"
#include "src/ui/desktop/calibration/rom_info.h"
#include "src/ui/desktop/widgets/calibration_maps.h"
#include "src/backend/calibration/session/rom_save.h"
#include "src/backend/checksum/checksum_selection.h"
#include "src/backend/checksum/dispatch.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"
#include "src/ui/desktop/hexedit/hexedit.h"
#include "src/ui/desktop/menu/testing/menu_snapshot.h"

namespace
{

const ApplicationIdentity kTestApplication{.name = "FastECU", .title = "FastECU", .version = "0.1.0-beta.5"};

constexpr auto kTcuChooserText = "Choose which option";
constexpr auto kTcuIgnitionText = "Turn ignition ON and press OK to start initializing connection to TCU";
constexpr auto kLegacyEcuIgnitionText = "Turn ignition ON and press OK to start initializing connection to ECU";
constexpr auto kNoChecksumModuleText = "WARNING! There is no checksum module for this ROM!";
constexpr auto kNoFileSelectedText = "No file selected!";
constexpr auto kPortableEcuIgnitionText = "Turn ignition ON and press OK to start initializing the ECU connection.";
constexpr auto kContinueWithoutDefinitionText = "Continue without definition file";

class ModalDriver final : public QObject
{

  public:
    explicit ModalDriver(QString chooser_choice) : chooser_choice_(std::move(chooser_choice))
    {
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, &ModalDriver::drive);
    }

    void start()
    {
        elapsed_.start();
        timer_.start();
    }

    void stop()
    {
        timer_.stop();
    }

    bool sawChooser() const
    {
        return saw_chooser_;
    }

    int ignitionCount() const
    {
        return ignition_count_;
    }

    int unexpectedFlashDialogCount() const
    {
        return unexpected_flash_dialog_count_;
    }

    int legacyEcuIgnitionCount() const
    {
        return legacy_ecu_ignition_count_;
    }

    int checksumWarningCount() const
    {
        return checksum_warning_count_;
    }
    int noFileSelectedCount() const
    {
        return no_file_selected_count_;
    }
    int missingDefinitionPromptCount() const
    {
        return missing_definition_prompt_count_;
    }
    int portableEcuIgnitionCount() const
    {
        return portable_ecu_ignition_count_;
    }

    bool timedOut() const
    {
        return timed_out_;
    }

    // Texts of the message boxes no rule above recognised; each was accepted.
    QStringList acceptedTexts() const
    {
        return accepted_texts_;
    }

  private:
    void drive()
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *message_box = qobject_cast<QMessageBox *>(widget); message_box != nullptr)
            {
                if (message_box->text() == kTcuChooserText)
                {
                    saw_chooser_ = true;
                    if (chooser_choice_.isEmpty())
                    {
                        message_box->reject();
                        return;
                    }
                    for (QAbstractButton *button : message_box->buttons())
                    {
                        if (button->text() == chooser_choice_)
                        {
                            button->click();
                            return;
                        }
                    }
                }
                if (message_box->text() == kTcuIgnitionText)
                {
                    ++ignition_count_;
                    message_box->done(QMessageBox::Cancel);
                    return;
                }
                if (message_box->text() == kLegacyEcuIgnitionText)
                {
                    ++legacy_ecu_ignition_count_;
                    message_box->done(QMessageBox::Cancel);
                    return;
                }
                if (message_box->text().startsWith(kNoChecksumModuleText))
                {
                    ++checksum_warning_count_;
                    message_box->done(QMessageBox::Cancel);
                    return;
                }
                if (message_box->text() == kPortableEcuIgnitionText)
                {
                    ++portable_ecu_ignition_count_;
                    message_box->done(QMessageBox::Cancel);
                    return;
                }
                if (message_box->text() == kNoFileSelectedText)
                {
                    ++no_file_selected_count_;
                }

                accepted_texts_ << message_box->text();
                message_box->accept();
                return;
            }
            if (widget->inherits("fastecu::flash::FlashDialog"))
            {
                unexpected_flash_dialog_count_ = 1;
            }
        }

        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (dialog == nullptr || !dialog->isVisible())
            {
                continue;
            }
            for (QRadioButton *button : dialog->findChildren<QRadioButton *>())
            {
                if (button->text() == kContinueWithoutDefinitionText)
                {
                    ++missing_definition_prompt_count_;
                    dialog->reject(); // the continue-without path
                    return;
                }
            }
        }

        if (elapsed_.elapsed() > 3000)
        {
            timed_out_ = true;
            for (QWidget *widget : QApplication::topLevelWidgets())
            {
                if (auto *dialog = qobject_cast<QDialog *>(widget); dialog != nullptr)
                {
                    dialog->reject();
                }
            }
        }
    }

  private:
    QString chooser_choice_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    bool saw_chooser_ = false;
    int ignition_count_ = 0;
    int unexpected_flash_dialog_count_ = 0;
    int legacy_ecu_ignition_count_ = 0;
    int portable_ecu_ignition_count_ = 0;
    int checksum_warning_count_ = 0;
    int no_file_selected_count_ = 0;
    int missing_definition_prompt_count_ = 0;
    bool timed_out_ = false;
    QStringList accepted_texts_;
};

// Invoke the slot through Qt, as the menu dispatch does.
int startEcuOperations(MainWindow& window, const QString& cmd_type)
{
    int result = -1;
    if (!QMetaObject::invokeMethod(&window, "start_ecu_operations", Qt::DirectConnection, Q_RETURN_ARG(int, result),
                                   Q_ARG(QString, cmd_type)))
    {
        return -1;
    }
    return result;
}

// A synthetic image on disk for the open-file path.
QString writeRom(const QTemporaryDir& dir, const QString& name, char fill, int size = 64)
{
    const QString path = dir.filePath(name);
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly) || file.write(QByteArray(size, fill)) != size)
    {
        return {};
    }
    return path;
}

bool writeTextFile(const QString& path, const char *contents)
{
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    return file.write(contents) == static_cast<qint64>(std::strlen(contents));
}

QByteArray frame(std::initializer_list<int> values)
{
    QByteArray out;
    for (int v : values)
    {
        out.append(static_cast<char>(v));
    }
    return out;
}

// A valid SSM2 ECU init response carrying ECU ID 3152584006.
const QByteArray kEcuInit = frame({0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x6C});

// An action as the tests name it: `member` is its objectName in mainwindow.ui.
struct ActionName
{
    const char *member;
};

constexpr ActionName kToggleRealtime{"actionToggleRealtime"};
constexpr ActionName kLogToFile{"actionLogToFile"};
constexpr ActionName kConnectToEcu{"actionConnectToEcu"};
constexpr ActionName kDisconnectFromEcu{"actionDisconnectFromEcu"};
constexpr ActionName kReadRomFromEcu{"actionReadRomFromEcu"};
constexpr ActionName kTestWriteRomToEcu{"actionTestWriteRomToEcu"};
constexpr ActionName kWriteRomToEcu{"actionWriteRomToEcu"};
constexpr ActionName kDtcWindow{"actionDtcWindow"};
constexpr ActionName kBiuCommunication{"actionBiuCommunication"};
constexpr ActionName kTerminal{"actionTerminal"};

QAction *menuAction(MainWindow& window, const ActionName& name)
{
    return window.findChild<QAction *>(QString::fromLatin1(name.member));
}

bool triggerMenu(MainWindow& window, const ActionName& name)
{
    QAction *action = menuAction(window, name);
    if (action == nullptr)
    {
        return false;
    }
    action->trigger();
    return true;
}

// A recorded LOG_* line as UTF-8 text with its timestamp and linefeed flags,
// so a failed comparison prints readable text.
using LogLine = std::tuple<std::string, bool, bool>;

template <typename Recorder> std::vector<LogLine> logLines(const Recorder& recorder)
{
    std::vector<LogLine> lines;
    for (const auto& [message, timestamp, linefeed] : recorder.snapshot())
    {
        lines.emplace_back(message.toStdString(), timestamp, linefeed);
    }
    return lines;
}

// The services DesktopComposition builds in the real app. The channels are
// left unwired: tests spy on them.
struct TestServices
{
    explicit TestServices(const QString& config_root)
        : config_status(config.initialize(config_root.toStdString(), kTestApplication.version)),
          definition_catalogs(definition_service, config, file_system, events)
    {
    }

    MainWindowServices services()
    {
        return {
            .application = kTestApplication,
            .config = config,
            .definition_catalogs = definition_catalogs,
            .calibrations = calibrations,
            .rom_save = rom_save,
            .logger_model = logger_model,
            .logger_definitions = logger_definitions,
            .config_repository = file_repository,
            .file_action_events = events,
            .log = log_channel,
            .connection = adapter.connection(),
            .remote = remote_peer,
            .logging_engine = logging_engine,
        };
    }

    QtFileSystem file_system;
    QtResourceBundle resource_bundle;
    QtFileRepository file_repository;
    QtAtomicFileWriter file_writer;
    QtEventSink events;
    QtEventSink config_events;
    fastecu::config::ConfigSession config{file_system, resource_bundle, file_repository, config_events};
    fastecu::Status config_status; // declared after `config`: initialized from it
    fastecu::definition::DefinitionService definition_service{file_system, file_repository, file_writer};
    fastecu::desktop::definition::DefinitionCatalogSession definition_catalogs;
    fastecu::calibration::RomOpenUseCase rom_open{
        definition_catalogs, definition_service, file_repository, file_system, events, config};
    fastecu::calibration::CalibrationWorkspace calibrations{rom_open};
    fastecu::calibration::RomSaveUseCase rom_save{file_repository, events};
    fastecu::logging::LoggerModel logger_model;
    fastecu::logging::LoggerDefinitionService logger_definitions{file_repository, resource_bundle, file_writer};
    fastecu::ui::LogChannel log_channel;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.fake(); // null if the fake backend failed to start
    fastecu::ui::RemotePeer remote_peer;
    fastecu::desktop::logging::LoggingEngine logging_engine;
};

} // namespace

class MainWindowTest : public ::testing::Test
{

  public:
    static void SetUpTestSuite();

    // A direct session (no peer address) must never wait for a remote source.

    // Spec behavior change 1: "No file selected!" returns after the entry
    // reset started battery polling; it must now run the cleanup.

    // Characterization: only Subaru and Mitsubishi dispatch, but every make
    // gets the cleanup.

    // Spec behavior change 2: a read that produces no calibration releases
    // the slot it allocated instead of leaking it.

    // Spec behavior change 1, second early return: Cancel on the
    // no-checksum-module warning must also stop battery polling.

    // The write path's metadata refresh decides the kernel and MCU handed to
    // a real ECU. CalibrationOperationCoordinator's own suite pins its rules;
    // the window keeps one case for the owned coordinator and its status
    // label.

    // Synthetic fixtures reproduced legacy label/ID lookups in 6l-1.
    // These assertions pin the corrected identities and empty CSV cells.

  protected:
    void copyFixtureConfig(const QString& root)
    {
        const QString suffix = "/" + QString::fromStdString(kTestApplication.version) + "/config/";
        const QDir source{config_root_->path() + suffix};
        ASSERT_TRUE(QDir().mkpath(root + suffix));
        for (const QString& name : source.entryList(QDir::Files))
        {
            ASSERT_TRUE(QFile::copy(source.filePath(name), root + suffix + name));
        }
    }

    // Selects the fixture's last vehicle row using `protocol`.
    static void selectProtocol(MainWindow& window, const QString& protocol)
    {
        ASSERT_TRUE(window.configSession->select_by_protocol_name(protocol.toStdString()));
    }

    // Selects the fixture's first Subaru row using `protocol`. The flash tests
    // dispatch only for Subaru/Mitsubishi, and sub_ecu_denso_sh7058's last row
    // is a Nissan one.
    static void selectSubaruProtocol(MainWindow& window, const QString& protocol)
    {
        const auto vehicles = window.configSession->vehicles();
        const auto it = std::ranges::find_if(
            vehicles, [&](const fastecu::config::ResolvedCarModel& vehicle)
            { return vehicle.make == "Subaru" && vehicle.protocol_name == protocol.toStdString(); });
        ASSERT_TRUE(it != vehicles.end());
        ASSERT_TRUE(window.configSession->select_row(static_cast<std::size_t>(it - vehicles.begin())).has_value());
    }

    // Selects the fixture's first vehicle row of `make`.
    static void selectMake(MainWindow& window, const QString& make)
    {
        const auto vehicles = window.configSession->vehicles();
        const auto it = std::ranges::find(vehicles, make.toStdString(), &fastecu::config::ResolvedCarModel::make);
        ASSERT_TRUE(it != vehicles.end());
        ASSERT_TRUE(window.configSession->select_row(static_cast<std::size_t>(it - vehicles.begin())).has_value());
    }

    // Points the window at one open port on the given make and log transport.
    static void prepareConnect(MainWindow& window, FakeBackend& fake, const QString& make, const QString& transport)
    {
        window.vbatt_timer->stop();
        window.serial_ports = {"ttyUSB0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("ttyUSB0");
        ASSERT_NO_FATAL_FAILURE(selectMake(window, make));
        window.configSession->settings().selected_log_transport = transport.toStdString();
        window.configSession->settings().selected_log_protocol = "SSM";
        ON_CALL(fake, open_serial_port()).WillByDefault(::testing::Return(QString("ttyUSB0")));
    }

    // The logging setup loggingCapturesTargetForEachRun and
    // loggingUsesTheSessionLogProtocol share: an identified ECU, a
    // "Logging" menu action, and one enabled `log_protocol` value.
    static void installLoggingFixture(MainWindow& window, fastecu::logging::LoggerDefinition definition,
                                      fastecu::logging::LoggerSelection selection)
    {
        *window.loggerModel = fastecu::logging::LoggerModel{};
        window.loggerModel->install_definition(std::move(definition));
        window.loggerModel->set_selection(std::move(selection));
        window.loggerValues.initialize(*window.loggerModel);
    }

    // Runs Save As on the window's selection and drives its file picker once:
    // `on_picker` runs while the picker is open, then the picker is cancelled
    // or accepts `target`. With `fail`, `target` becomes a directory as the
    // picker accepts, so the write fails. Informational notices are closed.
    // False when the picker never opened or timed out.
    static bool driveSaveAs(MainWindow& window, const QString& target, bool cancel, bool fail,
                            const std::function<void()>& on_picker = {})
    {
        QTimer timer;
        timer.setInterval(5);
        bool handled = false;
        bool timed_out = false;
        QElapsedTimer deadline;
        deadline.start();
        QObject::connect(
            &timer, &QTimer::timeout, &window,
            [&]
            {
                for (auto *widget : QApplication::topLevelWidgets())
                {
                    auto *dialog = qobject_cast<QFileDialog *>(widget);
                    if (dialog == nullptr || !dialog->isVisible())
                    {
                        continue;
                    }
                    if (deadline.elapsed() > 3000)
                    {
                        timed_out = true;
                        dialog->reject();
                        return;
                    }
                    if (handled)
                    {
                        continue;
                    }
                    handled = true;
                    if (on_picker)
                    {
                        on_picker();
                    }
                    if (cancel)
                    {
                        dialog->reject();
                        return;
                    }
                    dialog->setDirectory(QFileInfo(target).absolutePath());
                    dialog->selectFile(QFileInfo(target).fileName());
                    if (auto *edit = dialog->findChild<QLineEdit *>("fileNameEdit"); edit != nullptr)
                    {
                        edit->setText(QFileInfo(target).fileName());
                    }
                    if (fail)
                    {
                        QObject::connect(dialog, &QDialog::accepted, &window, [target] { QDir().mkdir(target); });
                    }
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                }
                // Close the existing cancellation/failure informational notice.
                for (auto *widget : QApplication::topLevelWidgets())
                {
                    if (auto *box = qobject_cast<QMessageBox *>(widget); box != nullptr && box->isVisible())
                    {
                        box->accept();
                    }
                }
            });
        timer.start();
        window.save_calibration_file_as();
        timer.stop();
        if (timed_out)
        {
            qWarning() << "Save As dialog timed out for" << target;
        }
        return handled && !timed_out;
    }

    // Selects only the files-tree row at `index`, as a click would.
    static void selectFilesRow(MainWindow& window, int index)
    {
        QTreeWidget *files = window.ui->calibrationFilesTreeWidget;
        for (int i = 0; i < files->topLevelItemCount(); ++i)
        {
            files->topLevelItem(i)->setSelected(i == index);
        }
    }

    static QAction *prepareLogging(MainWindow& window, const QString& log_protocol)
    {
        window.vbatt_timer->stop();
        window.ecu_init_complete = true;
        window.protocol = log_protocol;
        QAction *action = menuAction(window, kToggleRealtime);
        installLoggingFixture(window,
                              {.parameters = {{.protocol = log_protocol.toStdString(),
                                               .id = "rpm",
                                               .name = "rpm",
                                               .address = "000010",
                                               .length = "1",
                                               .ecu_byte_index = "0",
                                               .ecu_bit = "0",
                                               .target = "ECU",
                                               .enabled = true,
                                               .conversions = {{"rpm", "x", "0", "0", "100", "1"}}}}},
                              {.protocol = log_protocol.toStdString(), .lower_panel_ids = {"rpm"}});
        return action;
    }

  public:
    static inline std::unique_ptr<QTemporaryDir> config_root_;

  protected:
    void check_explicitConfigRootLoadsFixtureAndProvisionsDirectories();
    void check_directSessionStartupNeverWaitsForARemoteSource();
    void check_windowLogLinesReachTheLogChannel();
    void check_windowEnablesFileLoggingThroughTheChannel();
    void check_directSessionStartupNeverRequestsTheRemoteWait();
    void check_externalLoggerMirrorsToTheRemotePeer();
    void check_peerStateChangesReachTheWindow();
    void check_handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling(QString choice,
                                                                                   int expected_ignition_count);
    void check_futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo(QString protocol);
    void check_representativePortableRoutesReachFactoryBeforeLegacyFallback(QString protocol);
    void check_writeWithoutASelectedCalibrationStopsVoltagePolling(QString command);
    void check_otherMakesSkipDispatchButStillRunCleanup();
    void check_readOfAnUnsupportedProtocolAddsNoCalibration();
    void check_cancellingTheChecksumWarningStopsVoltagePolling(QString command);
    void check_definitionlessOpenPromptsOnceAndAppliesPlaceholders();
    void check_closingAMiddleRomKeepsLaterRomsAddressable();
    void check_windowsOfAClosedRomAreInert();
    void check_hexEditorOutlivesItsRom();
    void check_closingARomClosesAllOfItsWindows();
    void check_viewStateIsKeptPerRom();
    void check_writePreparationRefreshesMetadataAndStatusLabel();
    void check_checksumAndSaveUseATemporaryImage();
    void check_calibrationLogsReachTheLogChannel();
    void check_saveAsChangesSourceAndTreeOnlyAfterSuccess();
    void check_saveAsUpdatesOriginalSessionAfterSelectionChanges();
    void check_selectableSignalEditsItsEmittingSession();
    void check_failedMapDecodeDoesNotOccupyAView();
    void check_windowPreservesInjectedLoggingFactory();
    void check_loggingCapturesTargetForEachRun();
    void check_chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation(bool protocol, bool accept);
    void check_definitionManagerRemovesSelectedRowsAndSavesSurvivingOrder();
    void check_numericWindowGeometryRestoresAndPersistsAcrossWindowStates();
    void check_acceptedVehicleChoiceSelectsTheRowAndSavesIt();
    void check_cancelledVehicleChoiceChangesNothing();
    void check_acceptedProtocolChoiceSelectsTheLastMatchingRow();
    void check_romFlashMethodSelectsTheLastMatchingRow();
    void check_unmatchedRomFlashMethodChangesNothing();
    void check_restoreLoggingUiStateUnchecksLogging();
    void check_setRealtimeStateChecksAndUnchecksLogging();
    void check_identificationDisablesLoggingAndConnectButNotDisconnect();
    void check_logToFileActionDrivesWriteDatalogToFile();
    void check_unresolvedProtocolRowLeavesReadAndWriteUnavailable();
    void check_loggingUsesTheSessionLogProtocol();
    void check_selectedSerialPortIsEmptyWithoutPorts();
    void check_dtcWindowWithoutAPortWarnsInsteadOfCrashing();
    void check_repeatedSaveFailuresLogOnceUntilASuccess();
    void check_biuWindowRemembersTheOpenedPort();
    void check_disconnectReturnsTheAdapterToIdle();
    void check_connectOnAnotherMakeDisconnectsWithoutIdentifying();
    void check_subaruKlineConnectIdentifiesOffTheUiThread();
    void check_subaruConnectThatNeverAnswersDisconnectsAndRestoresControls();
    void check_disconnectDuringIdentificationCancelsAndDropsTheResult();
    void check_loggingSelectionFailureSemanticsAndSupportPreservation();
    void check_loggingDefinitionFailureIsNonfatal();
    void check_unresolvedDisplaySlotsAreSkippedAndUpdateTheirOriginalLabels();
    void check_chooserDuplicateLabelIdentity(int tab, QString kind);
    void check_csvSharedIdProtocolIdentity();
    void check_loggingStartWaitsForIdentification(bool target_is_ecu);
    void check_batterySamplingDoesNotUseTheFacadeDuringIdentification();
    void check_windowDestructionJoinsIdentificationWithoutContinuingLogging();
    void check_connectStopsAnActiveLoggingWorkerBeforeIdentification();
    void check_connectionEntryPointsStopIdentification(QString entry_point);
    void check_nestedConnectDuringCapabilityNoticeKeepsEachContinuation();
    void check_menuMatchesTheGolden();
    void check_everyIconNamedByTheMenuResolves();
    void check_noTwoActionsShareAShortcutAndNoneLostItsBinding();
    void check_toolbarKeepsMenuActionsBeforeTheTransportWidgets();
    void check_aStaleOrMalformedMenuCfgIsIgnored();
    void check_everyMenuActionIsConnectedToTheWindow();
    void check_triggeringLogToFileReachesItsHandler();
    void check_tuneActionsEditTheSelectionThroughTheirOwnHandlers();
};

void MainWindowTest::SetUpTestSuite()
{
    if (config_root_)
    {
        return;
    }
    config_root_ = std::make_unique<QTemporaryDir>();
    ASSERT_TRUE(config_root_->isValid());
    // Keep the hex editor settings isolated from the real user store.
    QCoreApplication::setOrganizationName("FastECU-test");
    QCoreApplication::setApplicationName("mainwindow-test");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config_root_->path() + "/settings");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    // Pass the fixture root explicitly: Qt resolves the Windows home from
    // the account profile before trying HOME/USERPROFILE fallbacks.
    const QString config_dir =
        config_root_->path() + "/" + QString::fromStdString(kTestApplication.version) + "/config/";
    qInfo() << "Fixture config:" << config_dir << "Qt home:" << QDir::homePath();
    ASSERT_TRUE(QDir().mkpath(config_dir));
    ASSERT_TRUE(writeTextFile(config_dir + "fastecu.cfg",

                              R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <software_settings>
    <setting name="window_size">
      <value width="maximized"/>
      <value height="maximized"/>
    </setting>
    <setting name="toolbar_iconsize"><value data="32"/></setting>
    <setting name="serial_port"><value data="OpenPort 2.0"/></setting>
    <setting name="protocol_id"><value data="0"/></setting>
    <setting name="flash_transport"><value data="iso15765"/></setting>
    <setting name="log_transport"><value data="K-Line"/></setting>
    <setting name="log_protocol"><value data="SSM"/></setting>
    <setting name="primary_definition_base"><value data="romraider"/></setting>
    <setting name="calibration_files"/>
    <setting name="calibration_files_directory"><value data="calibrations/"/></setting>
    <setting name="use_romraider_definitions"><value data="disabled"/></setting>
    <setting name="romraider_definition_files"><value data=""/></setting>
    <setting name="use_ecuflash_definitions"><value data="disabled"/></setting>
    <setting name="ecuflash_definition_files_directory"><value data=""/></setting>
    <setting name="logger_definition_file"><value data="logger.cfg"/></setting>
    <setting name="datalog_files_directory"><value data="datalogs/"/></setting>
  </software_settings>
</config>
)"));
    ASSERT_TRUE(writeTextFile(config_dir + "logger.cfg",
                              R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <logger/>
</config>
)"));
    ASSERT_TRUE(writeTextFile(config_dir + "protocols.cfg",
                              R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <protocols>
    <protocol name="sub_tcu_denso_sh7058_can">
      <ecu>Denso TCU SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>n/a</checksum>
      <read>yes</read>
      <test_write>no</test_write>
      <write>yes</write>
      <flash_transport>iso15765,CAN</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <ecu_id_ascii>no</ecu_id_ascii>
      <ecu_id_addr/>
      <ecu_id_length/>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0</cal_id_addr>
      <cal_id_length>10</cal_id_length>
      <kernel>tcu_kernel.bin</kernel>
      <kernel_addr>0x100000</kernel_addr>
      <description>Denso TCU SH7058</description>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_can">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>yes</checksum>
      <read>yes</read>
      <test_write>yes</test_write>
      <write>yes</write>
      <flash_transport>iso15765,CAN</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0x2004</cal_id_addr>
      <cal_id_length>8</cal_id_length>
      <kernel>test-kernel.bin</kernel>
      <kernel_addr>0xFFFF3000</kernel_addr>
      <description>Denso SH7058 CAN</description>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_densocan">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>yes</checksum>
      <read>yes</read>
      <test_write>yes</test_write>
      <write>yes</write>
      <flash_transport>CAN</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0x2000</cal_id_addr>
      <cal_id_length>8</cal_id_length>
      <kernel>test-kernel.bin</kernel>
      <kernel_addr>0xFFFF3000</kernel_addr>
      <description>Denso SH7058 DensoCAN</description>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>yes</checksum>
      <read>yes</read>
      <test_write>no</test_write>
      <write>yes</write>
      <flash_transport>K-Line</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0x2004</cal_id_addr>
      <cal_id_length>8</cal_id_length>
      <kernel>test-kernel.bin</kernel>
      <kernel_addr>0xFFFF3000</kernel_addr>
      <description>Denso SH7058 K-Line</description>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_can_future">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>yes</checksum>
      <read>yes</read>
      <test_write>yes</test_write>
      <write>yes</write>
      <flash_transport>iso15765,CAN</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0x2004</cal_id_addr>
      <cal_id_length>8</cal_id_length>
      <kernel>test-kernel.bin</kernel>
      <kernel_addr>0xFFFF3000</kernel_addr>
      <description>Denso SH7058 CAN</description>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_densocan_extra">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>yes</checksum>
      <read>yes</read>
      <test_write>yes</test_write>
      <write>yes</write>
      <flash_transport>iso15765,CAN</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0x2004</cal_id_addr>
      <cal_id_length>8</cal_id_length>
      <kernel>test-kernel.bin</kernel>
      <kernel_addr>0xFFFF3000</kernel_addr>
      <description>Denso SH7058 CAN</description>
    </protocol>
    <protocol name="sub_ecu_not_a_real_protocol">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>yes</checksum>
      <read>yes</read>
      <test_write>yes</test_write>
      <write>yes</write>
      <flash_transport>iso15765,CAN</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0x2004</cal_id_addr>
      <cal_id_length>8</cal_id_length>
      <kernel>test-kernel.bin</kernel>
      <kernel_addr>0xFFFF3000</kernel_addr>
      <description>Denso SH7058 CAN</description>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_can_checksum_na">
      <ecu>Denso SH7058</ecu>
      <mcu>SH7058</mcu>
      <mode>OBD2</mode>
      <checksum>n/a</checksum>
      <read>yes</read>
      <test_write>yes</test_write>
      <write>yes</write>
      <flash_transport>iso15765,CAN</flash_transport>
      <log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol>
      <cal_id_ascii>yes</cal_id_ascii>
      <cal_id_addr>0x2004</cal_id_addr>
      <cal_id_length>8</cal_id_length>
      <kernel>test-kernel.bin</kernel>
      <kernel_addr>0xFFFF3000</kernel_addr>
      <description>Denso SH7058 CAN</description>
    </protocol>
  </protocols>
  <car_models>
    <car_model>
      <make>Subaru</make>
      <model>Test</model>
      <version>Test</version>
      <type>TCU</type>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_tcu_denso_sh7058_can</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>Can</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058_can</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>DensoCan</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058_densocan</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>KLine</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>Future</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058_can_future</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>Extra</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058_densocan_extra</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>Unsupported</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_not_a_real_protocol</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>ChecksumNa</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058_can_checksum_na</protocol>
    </car_model>
    <car_model>
      <make>Mitsubishi</make>
      <model>Colt</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058</protocol>
    </car_model>
    <car_model>
      <make>Nissan</make>
      <model>Test</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_denso_sh7058</protocol>
    </car_model>
    <car_model>
      <make>Subaru</make>
      <model>Orphan</model>
      <version>Test</version>
      <type/>
      <kw/>
      <hp/>
      <fuel/>
      <year/>
      <protocol>sub_ecu_orphan</protocol>
    </car_model>
  </car_models>
</config>
)"));
    const QString kernel_dir =
        config_root_->path() + "/" + QString::fromStdString(kTestApplication.version) + "/kernels/";
    ASSERT_TRUE(QDir().mkpath(kernel_dir));
    ASSERT_TRUE(writeTextFile(kernel_dir + "test-kernel.bin", "ABCD"));
    ASSERT_TRUE(writeTextFile(kernel_dir + "tcu_kernel.bin", "ABCD"));
}

void MainWindowTest::check_explicitConfigRootLoadsFixtureAndProvisionsDirectories()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();

    const QString version_dir = config_root_->path() + "/" + window.software_version + "/";
    const fastecu::config::ConfigPaths paths = window.configSession->provisioned_paths();
    ASSERT_EQ(paths.base_config_directory, config_root_->path().toStdString());
    ASSERT_EQ(paths.config_file, (version_dir + "config/fastecu.cfg").toStdString());
    ASSERT_EQ(window.configSession->vehicles().front().model, std::string("Test"));
    ASSERT_EQ(paths.syslog_files_directory, (version_dir + "syslogs/").toStdString());
    ASSERT_TRUE(QDir(version_dir + "syslogs").exists());
    ASSERT_TRUE(QDir(version_dir + "definitions").exists());
    ASSERT_TRUE(QFile::exists(QString::fromStdString(paths.config_file)));
}

TEST_F(MainWindowTest, explicitConfigRootLoadsFixtureAndProvisionsDirectories)
{
    ASSERT_NO_FATAL_FAILURE(check_explicitConfigRootLoadsFixtureAndProvisionsDirectories());
}

void MainWindowTest::check_directSessionStartupNeverWaitsForARemoteSource()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    EXPECT_CALL(*services.fake, waitForSource()).Times(0);
    MainWindow window{services.services()};
    constructor_driver.stop();
}

TEST_F(MainWindowTest, directSessionStartupNeverWaitsForARemoteSource)
{
    ASSERT_NO_FATAL_FAILURE(check_directSessionStartupNeverWaitsForARemoteSource());
}

void MainWindowTest::check_windowLogLinesReachTheLogChannel()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    fastecu::testing::SignalRecorder lines{&services.log_channel, &fastecu::ui::LogChannel::LOG_I};

    emit window.LOG_I("probe line", true, false);

    ASSERT_EQ(lines.count(), 1U);
    ASSERT_EQ(std::get<0>(lines.snapshot().at(0)), QString("probe line"));
    ASSERT_EQ(std::get<1>(lines.snapshot().at(0)), true);
    ASSERT_EQ(std::get<2>(lines.snapshot().at(0)), false);
}

TEST_F(MainWindowTest, windowLogLinesReachTheLogChannel)
{
    ASSERT_NO_FATAL_FAILURE(check_windowLogLinesReachTheLogChannel());
}

void MainWindowTest::check_windowEnablesFileLoggingThroughTheChannel()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    fastecu::testing::SignalRecorder enables{&services.log_channel, &fastecu::ui::LogChannel::enable_log_write_to_file};
    MainWindow window{services.services()};
    constructor_driver.stop();

    ASSERT_TRUE(std::ranges::any_of(enables.snapshot(), [](const auto& arguments) { return std::get<0>(arguments); }));
}

TEST_F(MainWindowTest, windowEnablesFileLoggingThroughTheChannel)
{
    ASSERT_NO_FATAL_FAILURE(check_windowEnablesFileLoggingThroughTheChannel());
}

void MainWindowTest::check_directSessionStartupNeverRequestsTheRemoteWait()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    fastecu::testing::SignalRecorder waits{&services.remote_peer, &fastecu::ui::RemotePeer::wait_requested};
    MainWindow window{services.services()};
    constructor_driver.stop();

    ASSERT_EQ(waits.count(), 0U);
}

TEST_F(MainWindowTest, directSessionStartupNeverRequestsTheRemoteWait)
{
    ASSERT_NO_FATAL_FAILURE(check_directSessionStartupNeverRequestsTheRemoteWait());
}

void MainWindowTest::check_externalLoggerMirrorsToTheRemotePeer()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    fastecu::testing::SignalRecorder lines{&services.remote_peer, &fastecu::ui::RemotePeer::log_window_message};
    fastecu::testing::SignalRecorder progress{&services.remote_peer, &fastecu::ui::RemotePeer::progress};

    // Private slots: call by name so the Windows link needs no mangled
    // private symbol (see startEcuOperations).
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "external_logger", Qt::DirectConnection,
                                          Q_ARG(QString, QString("mirrored line"))));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "external_logger_set_progressbar_value", Qt::DirectConnection,
                                          Q_ARG(int, 42)));

    ASSERT_EQ(lines.count(), 1U);
    ASSERT_EQ(std::get<0>(lines.snapshot().at(0)), QString("mirrored line"));
    ASSERT_EQ(progress.count(), 1U);
    ASSERT_EQ(std::get<0>(progress.snapshot().at(0)), 42);
}

TEST_F(MainWindowTest, externalLoggerMirrorsToTheRemotePeer)
{
    ASSERT_NO_FATAL_FAILURE(check_externalLoggerMirrorsToTheRemotePeer());
}

void MainWindowTest::check_peerStateChangesReachTheWindow()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    fastecu::testing::SignalRecorder debug_lines{&window, &MainWindow::LOG_D};

    emit services.remote_peer.stateChanged(QRemoteObjectReplica::Valid, QRemoteObjectReplica::Default);

    ASSERT_TRUE(std::ranges::any_of(debug_lines.snapshot(), [](const auto& arguments)
                                    { return std::get<0>(arguments) == "Network connection established"; }));
}

TEST_F(MainWindowTest, peerStateChangesReachTheWindow)
{
    ASSERT_NO_FATAL_FAILURE(check_peerStateChangesReachTheWindow());
}

struct handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase
{
    std::string name;
    QString choice;
    int expected_ignition_count;
};
std::vector<handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase>
handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingRows()
{
    std::vector<handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase> rows;

    rows.push_back(
        handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase{"chooser-cancelled", QString(), 0});
    rows.push_back(handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase{"relearn-declined",
                                                                                           QString("Relearn"), 1});

    return rows;
}
class handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase>
{
};
INSTANTIATE_TEST_SUITE_P(
    Rows, handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingParameters,
    ::testing::ValuesIn(handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingRows()),
    [](const ::testing::TestParamInfo<handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase>& info)
    {
        auto name = info.param.name;
        for (char& c : name)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)))
            {
                c = '_';
            }
        }
        return name;
    });
void MainWindowTest::check_handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling(
    QString choice, int expected_ignition_count)
{

    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
    // Characterization: the call counts observed on master (before step
    // 6d) for both rows, pinned so the dispatch refactor cannot change them.
    EXPECT_CALL(*fake, reset_connection()).Times(3);
    EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(2);

    window.serial_ports = {"OpenPort 2.0"};
    window.serial_port_list->clear();
    window.serial_port_list->addItem("OpenPort 2.0");
    window.serial_port_list->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_tcu_denso_sh7058_can"));

    // The TCU log lines are relayed through MainWindow's own LOG_* signals.
    fastecu::testing::SignalRecorder info_lines{&window, &MainWindow::LOG_I};
    ModalDriver operation_driver{choice};
    operation_driver.start();
    ASSERT_EQ(startEcuOperations(window, "read"), 0);

    ASSERT_TRUE(operation_driver.sawChooser());
    ASSERT_EQ(operation_driver.ignitionCount(), expected_ignition_count);
    ASSERT_TRUE(!operation_driver.timedOut());
    ASSERT_EQ(operation_driver.unexpectedFlashDialogCount(), 0);

    fastecu::testing::process_events_for(std::chrono::milliseconds(window.vbatt_timer_timeout + 100));
    ASSERT_TRUE(!window.vbatt_timer->isActive());
    ASSERT_TRUE(window.calibrations_.empty());
    ASSERT_TRUE(services.calibrations.ids().empty());
    const QString expected_line = choice.isEmpty() ? "No option selected" : "Attempting TCU relearn";
    ASSERT_TRUE(std::ranges::any_of(info_lines.snapshot(),
                                    [&](const auto& arguments) { return std::get<0>(arguments) == expected_line; }));
}

TEST_P(handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingParameters,
       handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling)
{
    ASSERT_NO_FATAL_FAILURE(check_handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling(
        GetParam().choice, GetParam().expected_ignition_count));
}

struct futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase
{
    std::string name;
    QString protocol;
};
std::vector<futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase>
futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoRows()
{
    std::vector<futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase> rows;

    rows.push_back(futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase{
        "future-can", QString("sub_ecu_denso_sh7058_can_future")});
    rows.push_back(futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase{
        "extra-densocan", QString("sub_ecu_denso_sh7058_densocan_extra")});

    return rows;
}
class futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase>
{
};
INSTANTIATE_TEST_SUITE_P(
    Rows, futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoParameters,
    ::testing::ValuesIn(futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoRows()),
    [](const ::testing::TestParamInfo<futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase>& info)
    {
        auto name = info.param.name;
        for (char& c : name)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)))
            {
                c = '_';
            }
        }
        return name;
    });
void MainWindowTest::check_futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo(QString protocol)
{

    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
    window.serial_ports = {"OpenPort 2.0"};
    window.serial_port_list->clear();
    window.serial_port_list->addItem("OpenPort 2.0");
    window.serial_port_list->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, protocol));

    ModalDriver operation_driver{QString()};
    operation_driver.start();
    ASSERT_EQ(startEcuOperations(window, "read"), 0);
    operation_driver.stop();

    ASSERT_TRUE(!operation_driver.timedOut());
    ASSERT_EQ(operation_driver.legacyEcuIgnitionCount(), 0);
    ASSERT_EQ(operation_driver.portableEcuIgnitionCount(), 0);
    ASSERT_EQ(operation_driver.unexpectedFlashDialogCount(), 0);
}

TEST_P(futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoParameters,
       futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo)
{
    ASSERT_NO_FATAL_FAILURE(check_futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo(GetParam().protocol));
}

struct representativePortableRoutesReachFactoryBeforeLegacyFallbackCase
{
    std::string name;
    QString protocol;
};
std::vector<representativePortableRoutesReachFactoryBeforeLegacyFallbackCase>
representativePortableRoutesReachFactoryBeforeLegacyFallbackRows()
{
    std::vector<representativePortableRoutesReachFactoryBeforeLegacyFallbackCase> rows;

    rows.push_back(representativePortableRoutesReachFactoryBeforeLegacyFallbackCase{
        "petrol", QString("sub_ecu_denso_sh7058_can")});
    rows.push_back(representativePortableRoutesReachFactoryBeforeLegacyFallbackCase{
        "densocan", QString("sub_ecu_denso_sh7058_densocan")});
    // Wave 6b-2: the Denso SH705x K-Line family (sub_ecu_denso_sh7055_04*
    // and sub_ecu_denso_sh7058*) moved off FlashEcuSubaruDensoSH705xKline
    // onto this same portable factory path; see
    // exactDensoKlineIdsStillDispatchToTheLegacyKlineDialog in prior
    // revisions of this file for the characterization test this replaces.
    rows.push_back(representativePortableRoutesReachFactoryBeforeLegacyFallbackCase{"denso_sh705x_kline",
                                                                                    QString("sub_ecu_denso_sh7058")});

    return rows;
}
class representativePortableRoutesReachFactoryBeforeLegacyFallbackParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<representativePortableRoutesReachFactoryBeforeLegacyFallbackCase>
{
};
INSTANTIATE_TEST_SUITE_P(
    Rows, representativePortableRoutesReachFactoryBeforeLegacyFallbackParameters,
    ::testing::ValuesIn(representativePortableRoutesReachFactoryBeforeLegacyFallbackRows()),
    [](const ::testing::TestParamInfo<representativePortableRoutesReachFactoryBeforeLegacyFallbackCase>& info)
    {
        auto name = info.param.name;
        for (char& c : name)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)))
            {
                c = '_';
            }
        }
        return name;
    });
void MainWindowTest::check_representativePortableRoutesReachFactoryBeforeLegacyFallback(QString protocol)
{

    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
    window.serial_ports = {"OpenPort 2.0"};
    window.serial_port_list->clear();
    window.serial_port_list->addItem("OpenPort 2.0");
    window.serial_port_list->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, protocol));

    ModalDriver operation_driver{QString()};
    operation_driver.start();
    ASSERT_EQ(startEcuOperations(window, "read"), 0);
    operation_driver.stop();

    ASSERT_TRUE(!operation_driver.timedOut());
    ASSERT_EQ(operation_driver.legacyEcuIgnitionCount(), 0);
    ASSERT_EQ(operation_driver.portableEcuIgnitionCount(), 1);
}

TEST_P(representativePortableRoutesReachFactoryBeforeLegacyFallbackParameters,
       representativePortableRoutesReachFactoryBeforeLegacyFallback)
{
    ASSERT_NO_FATAL_FAILURE(check_representativePortableRoutesReachFactoryBeforeLegacyFallback(GetParam().protocol));
}

// Write and Test Write share one preflight, so each of its early returns is
// pinned for both commands.
struct writeCommandCase
{
    std::string name;
    QString command;
};
std::vector<writeCommandCase> writeCommandRows()
{
    return {writeCommandCase{"write", "write"}, writeCommandCase{"test_write", "test_write"}};
}
std::string writeCommandName(const ::testing::TestParamInfo<writeCommandCase>& info)
{
    return info.param.name;
}

class writeWithoutASelectedCalibrationStopsVoltagePollingParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<writeCommandCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, writeWithoutASelectedCalibrationStopsVoltagePollingParameters,
                         ::testing::ValuesIn(writeCommandRows()), writeCommandName);
void MainWindowTest::check_writeWithoutASelectedCalibrationStopsVoltagePolling(QString command)
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
    window.serial_ports = {"OpenPort 2.0"};
    window.serial_port_list->clear();
    window.serial_port_list->addItem("OpenPort 2.0");
    window.serial_port_list->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can"));

    ModalDriver operation_driver{QString()};
    operation_driver.start();
    ASSERT_EQ(startEcuOperations(window, command), 0);
    operation_driver.stop();

    ASSERT_TRUE(!operation_driver.timedOut());
    ASSERT_EQ(operation_driver.noFileSelectedCount(), 1);
    ASSERT_TRUE(!window.vbatt_timer->isActive());
}

TEST_P(writeWithoutASelectedCalibrationStopsVoltagePollingParameters,
       writeWithoutASelectedCalibrationStopsVoltagePolling)
{
    ASSERT_NO_FATAL_FAILURE(check_writeWithoutASelectedCalibrationStopsVoltagePolling(GetParam().command));
}

void MainWindowTest::check_otherMakesSkipDispatchButStillRunCleanup()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
    window.serial_ports = {"OpenPort 2.0"};
    window.serial_port_list->clear();
    window.serial_port_list->addItem("OpenPort 2.0");
    window.serial_port_list->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectMake(window, "Nissan"));

    ModalDriver operation_driver{QString()};
    operation_driver.start();
    ASSERT_EQ(startEcuOperations(window, "read"), 0);
    operation_driver.stop();

    ASSERT_TRUE(!operation_driver.timedOut());
    ASSERT_EQ(operation_driver.unexpectedFlashDialogCount(), 0);
    ASSERT_TRUE(!window.vbatt_timer->isActive());
}

TEST_F(MainWindowTest, otherMakesSkipDispatchButStillRunCleanup)
{
    ASSERT_NO_FATAL_FAILURE(check_otherMakesSkipDispatchButStillRunCleanup());
}

void MainWindowTest::check_readOfAnUnsupportedProtocolAddsNoCalibration()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();

    window.serial_ports = {"OpenPort 2.0"};
    window.serial_port_list->clear();
    window.serial_port_list->addItem("OpenPort 2.0");
    window.serial_port_list->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_not_a_real_protocol"));
    ASSERT_EQ(window.calibrations_.size(), std::size_t{0});

    ModalDriver operation_driver{QString()};
    operation_driver.start();
    ASSERT_EQ(startEcuOperations(window, "read"), 0);
    operation_driver.stop();

    ASSERT_TRUE(!operation_driver.timedOut());
    ASSERT_EQ(window.calibrations_.size(), std::size_t{0});
    ASSERT_TRUE(services.calibrations.ids().empty());
    ASSERT_EQ(window.ui->calibrationFilesTreeWidget->topLevelItemCount(), 0);
}

TEST_F(MainWindowTest, readOfAnUnsupportedProtocolAddsNoCalibration)
{
    ASSERT_NO_FATAL_FAILURE(check_readOfAnUnsupportedProtocolAddsNoCalibration());
}

class cancellingTheChecksumWarningStopsVoltagePollingParameters : public MainWindowTest,
                                                                  public ::testing::WithParamInterface<writeCommandCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, cancellingTheChecksumWarningStopsVoltagePollingParameters,
                         ::testing::ValuesIn(writeCommandRows()), writeCommandName);
void MainWindowTest::check_cancellingTheChecksumWarningStopsVoltagePolling(QString command)
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
    window.serial_ports = {"OpenPort 2.0"};
    window.serial_port_list->clear();
    window.serial_port_list->addItem("OpenPort 2.0");
    window.serial_port_list->setCurrentIndex(0);
    QTemporaryDir roms;
    const QString rom_path = writeRom(roms, "test.bin", '\x5a', 16);
    ASSERT_TRUE(!rom_path.isEmpty());
    ModalDriver open_driver{QString()};
    open_driver.start();
    ASSERT_EQ(window.open_calibration_file(rom_path), 0);
    open_driver.stop();
    ASSERT_EQ(open_driver.missingDefinitionPromptCount(), 1);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can_checksum_na"));

    ModalDriver operation_driver{QString()};
    operation_driver.start();
    ASSERT_EQ(startEcuOperations(window, command), 0);
    operation_driver.stop();

    ASSERT_TRUE(!operation_driver.timedOut());
    ASSERT_EQ(operation_driver.checksumWarningCount(), 1);
    ASSERT_TRUE(std::ranges::all_of(services.calibrations.find(window.calibrations_.front().id)->rom(),
                                    [](auto byte) { return byte == 0x5a; }));
    ASSERT_TRUE(!window.vbatt_timer->isActive());
}

TEST_P(cancellingTheChecksumWarningStopsVoltagePollingParameters, cancellingTheChecksumWarningStopsVoltagePolling)
{
    ASSERT_NO_FATAL_FAILURE(check_cancellingTheChecksumWarningStopsVoltagePolling(GetParam().command));
}

void MainWindowTest::check_definitionlessOpenPromptsOnceAndAppliesPlaceholders()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    QTemporaryDir roms;
    const QString path = writeRom(roms, "a.bin", '\x11');

    ModalDriver driver{QString()};
    driver.start();
    ASSERT_EQ(window.open_calibration_file(path), 0);
    driver.stop();

    ASSERT_TRUE(!driver.timedOut());
    ASSERT_EQ(driver.missingDefinitionPromptCount(), 1);
    ASSERT_EQ(window.calibrations_.size(), std::size_t{1});
    QTreeWidgetItem *rom_info = window.ui->calibrationDataTreeWidget->topLevelItem(0);
    ASSERT_EQ(rom_info->text(0), QString("ROM Info"));
    ASSERT_EQ(rom_info->child(0)->text(0), QString("XML ID: UnknownID"));
    ASSERT_EQ(rom_info->child(4)->text(0), "Make: " + QString::fromStdString(services.config.selected_vehicle()->make));
    ASSERT_EQ(window.calibrations_.front().view.missing_definition_make,
              std::optional<QString>(QString::fromStdString(services.config.selected_vehicle()->make)));
    ASSERT_EQ(services.calibrations.find(window.calibrations_.front().id)->source().display_name, std::string{"a.bin"});
    ASSERT_EQ(services.calibrations.ids().size(), std::size_t{1});
    ASSERT_EQ(window.ui->calibrationFilesTreeWidget->topLevelItem(0)->text(2),
              fastecu::ui::session_key_text(window.calibrations_.front().id));
}

TEST_F(MainWindowTest, definitionlessOpenPromptsOnceAndAppliesPlaceholders)
{
    ASSERT_NO_FATAL_FAILURE(check_definitionlessOpenPromptsOnceAndAppliesPlaceholders());
}

void MainWindowTest::check_closingAMiddleRomKeepsLaterRomsAddressable()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "c.bin", '\x0c')), 0);
    driver.stop();
    ASSERT_EQ(window.calibrations_.size(), std::size_t{3});
    const auto a = window.calibrations_.at(0).id;
    const auto c = window.calibrations_.at(2).id;
    QTreeWidget *files = window.ui->calibrationFilesTreeWidget;
    const QString c_key = files->topLevelItem(2)->text(2);

    for (int i = 0; i < files->topLevelItemCount(); ++i)
    {
        files->topLevelItem(i)->setSelected(i == 1);
    }
    window.close_calibration();

    ASSERT_EQ(window.calibrations_.size(), std::size_t{2});
    ASSERT_EQ(services.calibrations.ids(), (std::vector{a, c}));
    ASSERT_EQ(files->topLevelItemCount(), 2);
    ASSERT_EQ(files->topLevelItem(1)->text(2), c_key); // not renumbered
    ASSERT_TRUE(services.calibrations.find(c) != nullptr);
    ASSERT_EQ(services.calibrations.find(c)->source().display_name, std::string{"c.bin"});
    ASSERT_EQ(services.calibrations.find(c)->rom()[0], std::uint8_t{0x0c});
}

TEST_F(MainWindowTest, closingAMiddleRomKeepsLaterRomsAddressable)
{
    ASSERT_NO_FATAL_FAILURE(check_closingAMiddleRomKeepsLaterRomsAddressable());
}

void MainWindowTest::check_windowsOfAClosedRomAreInert()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
    driver.stop();
    const auto b = window.calibrations_.at(1).id;
    window.close_calibration(); // b is selected after its open
    ASSERT_TRUE(services.calibrations.find(b) == nullptr);

    const QString stale = fastecu::ui::session_key_text(b) + ",0,Idle";
    auto *content = new QWidget;
    QMdiSubWindow *sub = window.ui->mdiArea->addSubWindow(content);
    sub->setObjectName(stale);
    content->setObjectName(stale);
    window.ui->mdiArea->setActiveSubWindow(sub);
    QObject destroyed_window;
    destroyed_window.setObjectName(stale);

    window.set_maptablewidget_items();
    window.selectable_combobox_item_changed("anything");
    window.checkbox_state_changed(2);
    window.close_calibration_map(&destroyed_window);

    ASSERT_EQ(window.calibrations_.size(), std::size_t{1});
    ASSERT_EQ(window.ui->calibrationFilesTreeWidget->topLevelItemCount(), 1);
}

TEST_F(MainWindowTest, windowsOfAClosedRomAreInert)
{
    ASSERT_NO_FATAL_FAILURE(check_windowsOfAClosedRomAreInert());
}

void MainWindowTest::check_hexEditorOutlivesItsRom()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
    driver.stop();

    window.show_hex_editor();
    window.close_calibration();

    ASSERT_EQ(window.findChildren<HexEdit *>().size(), qsizetype{1});
    ASSERT_TRUE(window.calibrations_.empty());
}

TEST_F(MainWindowTest, hexEditorOutlivesItsRom)
{
    ASSERT_NO_FATAL_FAILURE(check_hexEditorOutlivesItsRom());
}

void MainWindowTest::check_closingARomClosesAllOfItsWindows()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
    driver.stop();
    const QString a_key = fastecu::ui::session_key_text(window.calibrations_.at(0).id);
    const QString b_key = fastecu::ui::session_key_text(window.calibrations_.at(1).id);
    for (const QString& name : {a_key + ",0,X", a_key + ",1,Y", b_key + ",0,Z"})
    {
        auto *content = new QWidget;
        QMdiSubWindow *sub = window.ui->mdiArea->addSubWindow(content);
        sub->setObjectName(name);
    }
    // The data tree still shows b; select a's row directly, as keyboard
    // navigation would, without rebuilding the data tree.
    QTreeWidget *files = window.ui->calibrationFilesTreeWidget;
    files->topLevelItem(0)->setSelected(true);
    files->topLevelItem(1)->setSelected(false);

    window.close_calibration();

    QStringList remaining;
    for (QMdiSubWindow *sub : window.ui->mdiArea->subWindowList())
    {
        remaining << sub->objectName();
    }
    ASSERT_EQ(remaining, QStringList{b_key + ",0,Z"});
}

TEST_F(MainWindowTest, closingARomClosesAllOfItsWindows)
{
    ASSERT_NO_FATAL_FAILURE(check_closingARomClosesAllOfItsWindows());
}

void MainWindowTest::check_viewStateIsKeptPerRom()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
    driver.stop();
    QTreeWidget *files = window.ui->calibrationFilesTreeWidget;
    QTreeWidget *data = window.ui->calibrationDataTreeWidget;
    const auto select_rom = [&](int row)
    {
        for (int i = 0; i < files->topLevelItemCount(); ++i)
        {
            files->topLevelItem(i)->setSelected(i == row);
        }
        window.calibration_files_treewidget_item_selected(files->topLevelItem(row));
    };

    select_rom(0);
    window.calibration_data_treewidget_item_expanded(data->topLevelItem(0)); // ROM Info
    select_rom(1);
    ASSERT_TRUE(!data->topLevelItem(0)->isExpanded());
    select_rom(0);
    ASSERT_TRUE(data->topLevelItem(0)->isExpanded());
    ASSERT_TRUE(window.calibrations_.at(0).view.rom_info_expanded);
    ASSERT_TRUE(!window.calibrations_.at(1).view.rom_info_expanded);
}

TEST_F(MainWindowTest, viewStateIsKeptPerRom)
{
    ASSERT_NO_FATAL_FAILURE(check_viewStateIsKeptPerRom());
}

void MainWindowTest::check_writePreparationRefreshesMetadataAndStatusLabel()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    // The Subaru K-Line row's protocol is also used by a later Nissan row,
    // which the empty-method fill reselects.
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    window.status_bar_ecu_label->setText("stale");
    fastecu::calibration::CalibrationSession session(
        fastecu::calibration::SessionId{41},
        fastecu::calibration::SessionContents{
            .source = {.display_name = "d.bin", .path = "/d.bin"},
            .rom = std::vector<std::uint8_t>(16, 0),
            .definition =
                fastecu::calibration::ResolvedDefinition{
                    .id = "D", .definition = {.format = fastecu::definition::DefinitionFormat::EcuFlash}},
        });
    ASSERT_EQ(fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(session), fastecu::ui::RomInfoRow::FlashMethod),
              QString(""));

    // The 16-byte image draws the checksum command's bad-size notice, which
    // the driver accepts.
    const std::optional<fastecu::ui::PreparedWrite> prepared =
        window.calibration_operations_->prepare_write(&session, "/kernels/");
    driver.stop();

    ASSERT_TRUE(!driver.timedOut());
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(window.status_bar_ecu_label->text().toStdString(), std::string{"Denso SH7058 K-Line "});
    EXPECT_EQ(services.config.selected_vehicle()->make, std::string{"Nissan"});
    EXPECT_EQ(session.protocol().flash_method, std::string{"sub_ecu_denso_sh7058"});
    EXPECT_EQ(fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(session), fastecu::ui::RomInfoRow::FlashMethod)
                  .toStdString(),
              std::string{"sub_ecu_denso_sh7058"});
    EXPECT_EQ(session.protocol().mcu_type, std::string{"SH7058"});
    EXPECT_EQ(prepared->kernel_path, std::string{"/kernels/test-kernel.bin"});
    EXPECT_EQ(prepared->display_filename, std::string{"d.bin"});
    EXPECT_THAT(prepared->image, ::testing::ElementsAreArray(session.rom()));
}

TEST_F(MainWindowTest, writePreparationRefreshesMetadataAndStatusLabel)
{
    ASSERT_NO_FATAL_FAILURE(check_writePreparationRefreshesMetadataAndStatusLabel());
}

void MainWindowTest::check_checksumAndSaveUseATemporaryImage()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    QTemporaryDir files;
    ASSERT_TRUE(writeTextFile(
        files.path() + "/definition.xml",
        R"(<rom><romid><xmlid>SAVE</xmlid><flashmethod>sub_ecu_denso_sh7058</flashmethod></romid></rom>)"));
    services.config.settings().primary_definition_base = "ecuflash";
    services.config.settings().use_ecuflash_definitions = "enabled";
    services.config.settings().ecuflash_definition_files_directory = files.path().toStdString();
    ASSERT_TRUE(
        services.definition_catalogs.refresh_index(fastecu::definition::DefinitionFormat::EcuFlash).has_value());
    const QString path = files.path() + "/save.bin";
    const auto opened = services.calibrations.adopt_read_image({
        .rom = bytes::Bytes(1024UZ * 1024, 0),
        .filename = path.toStdString(),
        .rom_id = "SAVE",
        .protocol_name = "sub_ecu_denso_sh7058",
    });
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(window.add_calibration(opened->id));
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    auto *session = services.calibrations.find(opened->id);
    ASSERT_TRUE(session != nullptr);
    ASSERT_TRUE(session->definition() != nullptr);
    ASSERT_TRUE(session->write_bytes(0, bytes::Bytes{1}).has_value());
    const bytes::Bytes original(session->rom().begin(), session->rom().end());
    // The expected bytes come from the backend dispatcher, independently of
    // the coordinator's selection plumbing.
    const fastecu::config::ResolvedCarModel& vehicle = *services.config.selected_vehicle();
    const fastecu::checksum::ChecksumCorrectionOutcome correction = fastecu::checksum::apply_checksum_correction(
        original, {
                      .make = vehicle.make,
                      .checksum_flag = fastecu::config::protocol_field_or_placeholder(
                          vehicle, &fastecu::config::ProtocolEntry::checksum),
                      .flash_method = vehicle.protocol_name,
                      .mcu_type = session->protocol().mcu_type,
                      .rom_id = session->protocol().rom_id,
                  });
    ASSERT_EQ(correction.status, fastecu::checksum::ChecksumCorrectionOutcome::Status::FamilyRan);
    ASSERT_TRUE(correction.family_result.has_value());
    ASSERT_TRUE(correction.family_result->ok());
    const bytes::Bytes corrected = correction.family_result->romData;
    ASSERT_TRUE(corrected != original);
    ASSERT_TRUE(session->dirty());

    window.save_calibration_file();
    EXPECT_THAT(services.file_repository.read(path.toStdString()), fastecu::testing::IsOkAnd(corrected));
    ASSERT_TRUE(std::ranges::equal(session->rom(), original));
    ASSERT_TRUE(!session->dirty());
    ASSERT_TRUE(session->write_bytes(0, bytes::Bytes{2}).has_value());
    const bytes::Bytes edited(session->rom().begin(), session->rom().end());
    // A directory is a deterministic failed file write on every platform.
    session->mark_saved(files.path().toStdString());
    ASSERT_TRUE(session->write_bytes(0, bytes::Bytes{2}).has_value());
    const auto source = session->source();
    window.save_calibration_file();
    ASSERT_TRUE(session->source() == source);
    ASSERT_TRUE(session->dirty());
    ASSERT_TRUE(std::ranges::equal(session->rom(), edited));
    driver.stop();
    ASSERT_TRUE(!driver.timedOut());
}

TEST_F(MainWindowTest, checksumAndSaveUseATemporaryImage)
{
    ASSERT_NO_FATAL_FAILURE(check_checksumAndSaveUseATemporaryImage());
}

void MainWindowTest::check_calibrationLogsReachTheLogChannel()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    QTemporaryDir files;
    // A non-ASCII name shows the window decodes the coordinator's UTF-8.
    const QString path = writeRom(files, QString::fromUtf8("caf\xc3\xa9.bin"), '\x22');
    ASSERT_TRUE(!path.isEmpty());
    ASSERT_EQ(window.open_calibration_file(path), 0);
    driver.stop();
    ASSERT_TRUE(!driver.timedOut());
    auto *session = services.calibrations.find(window.calibrations_.front().id);
    ASSERT_TRUE(session != nullptr);
    auto protocol = session->protocol();
    protocol.mcu_type.clear(); // Unknown MCU: an error line and no checksum dialog.
    session->set_protocol(protocol);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    fastecu::testing::SignalRecorder debug{&services.log_channel, &fastecu::ui::LogChannel::LOG_D};
    fastecu::testing::SignalRecorder errors{&services.log_channel, &fastecu::ui::LogChannel::LOG_E};

    window.save_calibration_file();

    EXPECT_THAT(logLines(debug),
                ::testing::ElementsAre(LogLine{"Protocol: sub_ecu_denso_sh7058", true, true},
                                       LogLine{"Make: Subaru", true, true}, LogLine{"Checksum: yes", true, true},
                                       LogLine{"ecuCalDef->FileName: caf\xc3\xa9.bin", true, true},
                                       LogLine{"ecuCalDef->FullFileName: " + session->source().path, true, true}));
    EXPECT_THAT(logLines(errors), ::testing::ElementsAre(LogLine{"Unknown MCU type: ", true, true}));
}

TEST_F(MainWindowTest, calibrationLogsReachTheLogChannel)
{
    ASSERT_NO_FATAL_FAILURE(check_calibrationLogsReachTheLogChannel());
}

void MainWindowTest::check_saveAsChangesSourceAndTreeOnlyAfterSuccess()
{
    const bool native_disabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restore_dialogs =
        qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native_disabled); });
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    QTemporaryDir files;
    const QString original_path = writeRom(files, "original.bin", '\x11');
    ASSERT_EQ(window.open_calibration_file(original_path), 0);
    auto *session = services.calibrations.find(window.calibrations_.front().id);
    ASSERT_TRUE(session != nullptr);
    auto protocol = session->protocol();
    protocol.mcu_type.clear(); // Unknown MCU preserves bytes without a checksum dialog.
    session->set_protocol(protocol);
    ASSERT_TRUE(session->write_bytes(0, bytes::Bytes{9}).has_value());
    const auto original_source = session->source();
    QTreeWidgetItem *row = window.ui->calibrationFilesTreeWidget->topLevelItem(0);
    const QString original_label = row->text(0);
    driver.stop();

    ASSERT_TRUE(driveSaveAs(window, {}, true, false));
    ASSERT_TRUE(session->source() == original_source);
    ASSERT_TRUE(session->dirty());
    ASSERT_EQ(row->text(0), original_label);
    const QString blocked = files.path() + "/blocked.bin";
    ASSERT_TRUE(driveSaveAs(window, blocked, false, true));
    ASSERT_TRUE(session->source() == original_source);
    ASSERT_TRUE(session->dirty());
    ASSERT_EQ(row->text(0), original_label);
    ASSERT_TRUE(driveSaveAs(window, files.path() + "/renamed.", false, false));
    ASSERT_EQ(session->source().path, (files.path() + "/renamed.bin").toStdString());
    ASSERT_EQ(session->source().display_name, std::string{"renamed.bin"});
    ASSERT_TRUE(!session->dirty());
    ASSERT_TRUE(row->text(0).contains("renamed.bin"));
    const auto saved = services.file_repository.read(session->source().path);
    ASSERT_TRUE(saved.has_value());
    ASSERT_EQ(saved->at(0), std::uint8_t{9});
    ASSERT_EQ(session->rom()[0], std::uint8_t{9});
}

TEST_F(MainWindowTest, saveAsChangesSourceAndTreeOnlyAfterSuccess)
{
    ASSERT_NO_FATAL_FAILURE(check_saveAsChangesSourceAndTreeOnlyAfterSuccess());
}

// The picker is modal but the files tree can still change selection under
// it; the session selected when Save As started receives the save and the
// row label, never the row selected later.
void MainWindowTest::check_saveAsUpdatesOriginalSessionAfterSelectionChanges()
{
    const bool native_disabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restore_dialogs =
        qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native_disabled); });
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    QTemporaryDir files;
    ASSERT_EQ(window.open_calibration_file(writeRom(files, "a.bin", '\x0a')), 0);
    ASSERT_EQ(window.open_calibration_file(writeRom(files, "b.bin", '\x0b')), 0);
    driver.stop();
    ASSERT_EQ(window.calibrations_.size(), std::size_t{2});
    auto *a = services.calibrations.find(window.calibrations_.at(0).id);
    auto *b = services.calibrations.find(window.calibrations_.at(1).id);
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(b != nullptr);
    for (auto *session : {a, b})
    {
        auto protocol = session->protocol();
        protocol.mcu_type.clear(); // Unknown MCU preserves bytes without a checksum dialog.
        session->set_protocol(protocol);
    }
    QTreeWidgetItem *a_row = window.files_tree_item(a->id());
    QTreeWidgetItem *b_row = window.files_tree_item(b->id());
    ASSERT_TRUE(a_row != nullptr);
    ASSERT_TRUE(b_row != nullptr);
    const auto b_source = b->source();
    const std::string b_label = b_row->text(0).toStdString();
    ASSERT_NO_FATAL_FAILURE(selectFilesRow(window, 0));
    ASSERT_EQ(window.selected_calibration(), a);

    const QString target = files.path() + "/renamed.bin";
    ASSERT_TRUE(driveSaveAs(window, target, false, false, [&] { selectFilesRow(window, 1); }));

    EXPECT_EQ(a->source().path, target.toStdString());
    EXPECT_EQ(a->source().display_name, std::string{"renamed.bin"});
    EXPECT_EQ(a_row->text(0).toStdString(), std::string{"renamed.bin"});
    EXPECT_TRUE(b->source() == b_source);
    EXPECT_EQ(b_row->text(0).toStdString(), b_label);
    EXPECT_EQ(window.selected_calibration(), b);
    EXPECT_THAT(services.file_repository.read(target.toStdString()),
                fastecu::testing::IsOkAnd(::testing::Each(std::uint8_t{0x0a})));
}

TEST_F(MainWindowTest, saveAsUpdatesOriginalSessionAfterSelectionChanges)
{
    ASSERT_NO_FATAL_FAILURE(check_saveAsUpdatesOriginalSessionAfterSelectionChanges());
}

void MainWindowTest::check_selectableSignalEditsItsEmittingSession()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    window.show();
    QApplication::processEvents();
    QTemporaryDir files;
    ASSERT_TRUE(writeTextFile(files.path() + "/selector.xml", R"(
<rom><romid><xmlid>SELECT</xmlid></romid>
<table name="Mode" category="Controls" address="0" type="1D" sizex="1" sizey="1">
<scaling storagetype="bloblist" endian="big"><data name="off" value="00"/><data name="on" value="01"/></scaling>
</table></rom>)"));
    services.config.settings().primary_definition_base = "ecuflash";
    services.config.settings().use_ecuflash_definitions = "enabled";
    services.config.settings().ecuflash_definition_files_directory = files.path().toStdString();
    ASSERT_TRUE(
        services.definition_catalogs.refresh_index(fastecu::definition::DefinitionFormat::EcuFlash).has_value());
    const auto open_map = [&](const QString& name) -> CalibrationMaps *
    {
        const auto opened = services.calibrations.adopt_read_image({
            .rom = bytes::Bytes(16, 0),
            .filename = name.toStdString(),
            .rom_id = "SELECT",
        });
        if (!opened.has_value() || !window.add_calibration(opened->id))
        {
            return nullptr;
        }
        auto *file_tree = window.ui->calibrationFilesTreeWidget;
        for (int row = 0; row < file_tree->topLevelItemCount(); ++row)
        {
            file_tree->topLevelItem(row)->setSelected(row == file_tree->topLevelItemCount() - 1);
        }
        window.calibration_files_treewidget_item_selected(file_tree->topLevelItem(file_tree->topLevelItemCount() - 1));
        QTreeWidget *tree = window.ui->calibrationDataTreeWidget;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
        {
            auto *category = tree->topLevelItem(i);
            if (category->text(0) == "Controls" && category->childCount() != 0)
            {
                tree->setCurrentItem(category->child(0));
                window.calibration_data_treewidget_item_selected(category->child(0));
                const auto windows = window.ui->mdiArea->subWindowList();
                if (windows.isEmpty())
                {
                    return nullptr;
                }
                window.ui->mdiArea->setActiveSubWindow(windows.back());
                return qobject_cast<CalibrationMaps *>(windows.back()->widget());
            }
        }
        return nullptr;
    };
    CalibrationMaps *first = open_map(files.path() + "/first.bin");
    ASSERT_TRUE(first != nullptr);
    const auto first_id = services.calibrations.ids().front();
    CalibrationMaps *second = open_map(files.path() + "/second.bin");
    ASSERT_TRUE(second != nullptr);
    ASSERT_TRUE(first != second);
    const auto second_id = services.calibrations.ids().back();
    ASSERT_TRUE(window.ui->mdiArea->activeSubWindow()->widget() == second);

    // Emit from the inactive first map while the second ROM/window is selected.
    first->selectable_combobox_item_changed("enabled");

    ASSERT_EQ(services.calibrations.find(first_id)->rom()[0], std::uint8_t{1});
    ASSERT_TRUE(services.calibrations.find(first_id)->dirty());
    ASSERT_EQ(services.calibrations.find(second_id)->rom()[0], std::uint8_t{0});
    ASSERT_TRUE(!services.calibrations.find(second_id)->dirty());
    driver.stop();
    ASSERT_TRUE(!driver.timedOut());
}

TEST_F(MainWindowTest, selectableSignalEditsItsEmittingSession)
{
    ASSERT_NO_FATAL_FAILURE(check_selectableSignalEditsItsEmittingSession());
}

void MainWindowTest::check_failedMapDecodeDoesNotOccupyAView()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    QTemporaryDir files;
    ASSERT_EQ(window.open_calibration_file(writeRom(files, "bad.bin", '\x11')), 0);
    driver.stop();
    const auto id = window.calibrations_.front().id;
    auto *session = services.calibrations.find(id);
    ASSERT_TRUE(session != nullptr);
    fastecu::definition::RomDefinition definition{.format = fastecu::definition::DefinitionFormat::EcuFlash};
    definition.scalings.push_back({.name = "Raw"});
    fastecu::definition::CalibrationMap map;
    map.name = "Broken";
    map.category = "Controls";
    map.type = "1D";
    map.address = 1000; // Beyond the opened 16-byte image.
    map.x_size = 1;
    map.y_size = 1;
    map.storage_type = fastecu::definition::StorageType::Uint8;
    map.scaling_name = "Raw";
    definition.maps.push_back(map);
    *session = fastecu::calibration::CalibrationSession(
        id,
        {
            .source = session->source(),
            .rom = bytes::Bytes(16, 0),
            .definition = fastecu::calibration::ResolvedDefinition{.id = "BAD", .definition = std::move(definition)},
        });
    window.calibration_files_treewidget_item_selected(window.ui->calibrationFilesTreeWidget->topLevelItem(0));
    QTreeWidget *tree = window.ui->calibrationDataTreeWidget;
    QTreeWidgetItem *item = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
    {
        auto *category = tree->topLevelItem(i);
        if (category->text(0) == "Controls")
        {
            item = category->child(0);
        }
    }
    ASSERT_TRUE(item != nullptr);
    fastecu::testing::SignalRecorder errors(&window, &MainWindow::LOG_E);
    tree->setCurrentItem(item);
    window.calibration_data_treewidget_item_selected(item);
    ASSERT_TRUE(window.ui->mdiArea->subWindowList().isEmpty());
    ASSERT_TRUE(window.calibrations_.front().view.open_maps.empty());
    ASSERT_EQ(item->checkState(0), Qt::Unchecked);
    ASSERT_TRUE(!errors.snapshot().empty());
}

TEST_F(MainWindowTest, failedMapDecodeDoesNotOccupyAView)
{
    ASSERT_NO_FATAL_FAILURE(check_failedMapDecodeDoesNotOccupyAView());
}

void MainWindowTest::check_windowPreservesInjectedLoggingFactory()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    bool called = false;
    services.logging_engine.registerProtocol(
        "SSM",
        [&called](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
        {
            called = !snapshot.target_is_ecu;
            auto protocol = std::make_unique<ScriptedLoggingProtocol>();
            protocol->blockPollUntilCancelled();
            return protocol;
        });
    MainWindow window{services.services()};
    constructor_driver.stop();
    // This test checks ownership, not UI error dialogs. The next test
    // drives actual menu dispatch and checks the selected target.
    QObject::disconnect(&services.logging_engine, nullptr, &window, nullptr);
    auto session =
        fastecu::logging::make_logging_session(fastecu::logging::LoggingProtocolId::Ssm,
                                               {{.id = "rpm",
                                                 .address = 0x10,
                                                 .length = 1,
                                                 .raw_assembly = fastecu::logging::RawAssembly::UnsignedIntegerDecimal,
                                                 .from_byte_expression = "x",
                                                 .unit = "rpm",
                                                 .decimal_precision = 0}},
                                               {.poll_timeout = std::chrono::milliseconds{50},
                                                .car_silence_miss_threshold = 20,
                                                .reconnect_attempt_threshold = 100,
                                                .reconnect_retry_period = 20});
    ASSERT_TRUE(session);
    ASSERT_TRUE(services.logging_engine.start(
        {.protocolId = "SSM"}, {.session = std::move(*session), .response_offsets = {0}, .target_is_ecu = false}));
    services.logging_engine.stop();
    ASSERT_TRUE(called);
}

TEST_F(MainWindowTest, windowPreservesInjectedLoggingFactory)
{
    ASSERT_NO_FATAL_FAILURE(check_windowPreservesInjectedLoggingFactory());
}

void MainWindowTest::check_loggingCapturesTargetForEachRun()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    window.configSession->settings().selected_log_protocol = "SSM";
    QAction *action = prepareLogging(window, "SSM");
    std::vector<bool> targets;
    services.logging_engine.registerProtocol(
        "SSM",
        [&targets](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
        {
            targets.push_back(snapshot.target_is_ecu);
            auto protocol = std::make_unique<ScriptedLoggingProtocol>();
            protocol->blockPollUntilCancelled();
            return protocol;
        });
    for (bool target : {true, false})
    {
        window.ecu_radio_button->setAutoExclusive(false);
        window.ecu_radio_button->setChecked(target);
        // trigger() toggles a checkable action, as a click does: start
        // unchecked so the handler sees Logging switched on.
        action->setChecked(false);
        ASSERT_TRUE(triggerMenu(window, kToggleRealtime));
        ASSERT_TRUE(action->isChecked());
        ASSERT_TRUE(window.activeLoggingSnapshot.has_value());
        ASSERT_EQ(window.activeLoggingSnapshot->target_is_ecu, target);
        services.logging_engine.stop();
    }
    ASSERT_EQ(targets, (std::vector<bool>{true, false}));
}

TEST_F(MainWindowTest, loggingCapturesTargetForEachRun)
{
    ASSERT_NO_FATAL_FAILURE(check_loggingCapturesTargetForEachRun());
}

struct chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase
{
    std::string name;
    bool protocol;
    bool accept;
};
std::vector<chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase>
chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationRows()
{
    std::vector<chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase> rows;

    rows.push_back(chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase{"vehicle-accept", false, true});
    rows.push_back(chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase{"vehicle-cancel", false, false});
    rows.push_back(chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase{"protocol-accept", true, true});
    rows.push_back(chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase{"protocol-cancel", true, false});

    return rows;
}
class chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase>
{
};
INSTANTIATE_TEST_SUITE_P(
    Rows, chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationParameters,
    ::testing::ValuesIn(chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationRows()),
    [](const ::testing::TestParamInfo<chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase>& info)
    {
        auto name = info.param.name;
        for (char& c : name)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)))
            {
                c = '_';
            }
        }
        return name;
    });
void MainWindowTest::check_chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation(bool protocol, bool accept)
{

    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_NO_FATAL_FAILURE(copyFixtureConfig(root.path()));
    TestServices services{root.path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.config.select_row(0).has_value());
    ASSERT_TRUE(services.config.save().has_value());
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    MainWindow window{services.services()};
    constructor_driver.stop();
    const auto target = services.config.vehicles()[1];
    std::size_t expected = 1;
    if (protocol)
    {
        for (std::size_t row = 0; row < services.config.vehicles().size(); ++row)
        {
            if (services.config.vehicles()[row].protocol_name == target.protocol_name)
            {
                expected = row;
            }
        }
    }
    QTimer driver;
    QElapsedTimer deadline;
    bool driven = false;
    bool unexpected = false;
    bool timed_out = false;
    QObject::connect(&driver, &QTimer::timeout,
                     [&]
                     {
                         auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                         if (deadline.elapsed() > 3000)
                         {
                             timed_out = true;
                             if (dialog)
                             {
                                 dialog->reject();
                             }
                             return;
                         }
                         if (!dialog)
                         {
                             return;
                         }
                         const char *expected_class = protocol ? "ProtocolSelect" : "VehicleSelect";
                         if (!dialog->inherits(expected_class))
                         {
                             unexpected = true;
                             dialog->reject();
                             return;
                         }
                         auto selectText = [](QTreeWidget *tree, const QString& text)
                         {
                             if (!tree)
                             {
                                 return false;
                             }
                             for (int i = 0; i < tree->topLevelItemCount(); ++i)
                             {
                                 if (tree->topLevelItem(i)->text(0) == text)
                                 {
                                     tree->setCurrentItem(tree->topLevelItem(i));
                                     return true;
                                 }
                             }
                             return false;
                         };
                         bool selected = false;
                         if (protocol)
                         {
                             selected = selectText(dialog->findChild<QTreeWidget *>("treeWidget"),
                                                   QString::fromStdString(target.protocol_name));
                         }
                         else
                         {
                             selected = selectText(dialog->findChild<QTreeWidget *>("car_make_tree_widget"),
                                                   QString::fromStdString(target.make)) &&
                                        selectText(dialog->findChild<QTreeWidget *>("car_model_tree_widget"),
                                                   QString::fromStdString(target.model));
                             auto *versions = dialog->findChild<QTreeWidget *>("car_version_tree_widget");
                             selected = selected && versions != nullptr;
                             bool row_found = false;
                             if (versions)
                             {
                                 for (int i = 0; i < versions->topLevelItemCount(); ++i)
                                 {
                                     auto *item = versions->topLevelItem(i);
                                     if (item->text(12) == "1")
                                     {
                                         versions->setCurrentItem(item);
                                         row_found = true;
                                         break;
                                     }
                                 }
                             }
                             selected = selected && row_found;
                         }
                         auto *button = dialog->findChild<QPushButton *>(accept ? "select_button" : "cancel_button");
                         driven = selected && button && button->isEnabled();
                         if (driven)
                         {
                             button->click();
                         }
                         else
                         {
                             unexpected = true;
                             dialog->reject();
                         }
                     });
    deadline.start();
    driver.start(5);
    if (protocol)
    {
        window.select_protocol();
    }
    else
    {
        window.select_vehicle();
    }
    driver.stop();
    ASSERT_TRUE(driven);
    ASSERT_TRUE(!unexpected);
    ASSERT_TRUE(!timed_out);
    ASSERT_EQ(*services.config.selected_row(), accept ? expected : std::size_t{0});
    TestServices reread{root.path()};
    ASSERT_TRUE(reread.config_status.has_value());
    ASSERT_EQ(*reread.config.selected_row(), accept ? expected : std::size_t{0});
}

TEST_P(chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationParameters,
       chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation)
{
    ASSERT_NO_FATAL_FAILURE(
        check_chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation(GetParam().protocol, GetParam().accept));
}

void MainWindowTest::check_definitionManagerRemovesSelectedRowsAndSavesSurvivingOrder()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_NO_FATAL_FAILURE(copyFixtureConfig(root.path()));
    TestServices services{root.path()};
    ASSERT_TRUE(services.config_status.has_value());
    services.config.settings().romraider_definition_files = {"/first.xml", "/middle.xml", "/last.xml"};
    ASSERT_TRUE(services.config.save().has_value());
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    MainWindow window{services.services()};
    constructor_driver.stop();
    QTimer driver;
    QElapsedTimer deadline;
    bool driven = false;
    bool unexpected = false;
    bool unchanged_without_selection = false;
    QStringList displayed;
    QObject::connect(&driver, &QTimer::timeout,
                     [&]
                     {
                         auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                         if (!dialog)
                         {
                             return;
                         }
                         if (deadline.elapsed() > 3000 || dialog->objectName() != "ecu_definition_manager_dialog")
                         {
                             unexpected = true;
                             dialog->reject();
                             return;
                         }
                         auto *list = dialog->findChild<QListWidget *>("ecu_definition_files_list");
                         QPushButton *remove = nullptr;
                         for (auto *button : dialog->findChildren<QPushButton *>())
                         {
                             if (button->text() == "Remove file")
                             {
                                 remove = button;
                             }
                         }
                         if (!list || !remove || list->count() != 3)
                         {
                             unexpected = true;
                             dialog->reject();
                             return;
                         }
                         list->clearSelection();
                         remove->click();
                         unchanged_without_selection =
                             services.config.settings().romraider_definition_files ==
                             std::vector<std::string>{"/first.xml", "/middle.xml", "/last.xml"};
                         list->item(1)->setSelected(true);
                         remove->click();
                         for (int i = 0; i < list->count(); ++i)
                         {
                             displayed.append(list->item(i)->text());
                         }
                         driven = true;
                         dialog->reject();
                     });
    deadline.start();
    driver.start(5);
    window.ecu_definition_manager();
    driver.stop();
    ASSERT_TRUE(driven);
    ASSERT_TRUE(!unexpected);
    ASSERT_TRUE(unchanged_without_selection);
    ASSERT_EQ(displayed, (QStringList{"/first.xml", "/last.xml"}));
    const std::vector<std::string> expected{"/first.xml", "/last.xml"};
    ASSERT_EQ(services.config.settings().romraider_definition_files, expected);
    TestServices reread{root.path()};
    ASSERT_TRUE(reread.config_status.has_value());
    ASSERT_EQ(reread.config.settings().romraider_definition_files, expected);
}

TEST_F(MainWindowTest, definitionManagerRemovesSelectedRowsAndSavesSurvivingOrder)
{
    ASSERT_NO_FATAL_FAILURE(check_definitionManagerRemovesSelectedRowsAndSavesSurvivingOrder());
}

void MainWindowTest::check_numericWindowGeometryRestoresAndPersistsAcrossWindowStates()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_NO_FATAL_FAILURE(copyFixtureConfig(root.path()));
    TestServices services{root.path()};
    ASSERT_TRUE(services.config_status.has_value());
    services.config.settings().window_width = "900";
    services.config.settings().window_height = "700";
    ASSERT_TRUE(services.config.save().has_value());
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_EQ(window.size(), QSize(900, 700));
    window.show();
    window.resize(950, 750);
    QCoreApplication::processEvents();
    ASSERT_EQ(window.size(), QSize(950, 750));
    TestServices resized{root.path()};
    ASSERT_TRUE(resized.config_status.has_value());
    ASSERT_EQ(resized.config.settings().window_width, std::string("950"));
    ASSERT_EQ(resized.config.settings().window_height, std::string("750"));
    window.showMaximized();
    QCoreApplication::processEvents();
    ASSERT_EQ(services.config.settings().window_width, std::string("maximized"));
    ASSERT_EQ(services.config.settings().window_height, std::string("maximized"));
    TestServices maximized{root.path()};
    ASSERT_TRUE(maximized.config_status.has_value());
    ASSERT_EQ(maximized.config.settings().window_width, std::string("maximized"));
    window.showNormal();
    QCoreApplication::processEvents();
    ASSERT_EQ(services.config.settings().window_width, std::to_string(window.width()));
    ASSERT_EQ(services.config.settings().window_height, std::to_string(window.height()));
    TestServices restored{root.path()};
    ASSERT_TRUE(restored.config_status.has_value());
    ASSERT_EQ(restored.config.settings().window_width, services.config.settings().window_width);
    ASSERT_EQ(restored.config.settings().window_height, services.config.settings().window_height);
}

TEST_F(MainWindowTest, numericWindowGeometryRestoresAndPersistsAcrossWindowStates)
{
    ASSERT_NO_FATAL_FAILURE(check_numericWindowGeometryRestoresAndPersistsAcrossWindowStates());
}

void MainWindowTest::check_acceptedVehicleChoiceSelectsTheRowAndSavesIt()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    const std::string flash_transport = services.config.settings().selected_flash_transport;
    const std::string log_transport = services.config.settings().selected_log_transport;

    window.apply_vehicle_choice(QDialog::Accepted, 1);

    ASSERT_EQ(*services.config.selected_row(), std::size_t{1});
    ASSERT_EQ(services.config.settings().selected_log_protocol, std::string("SSM"));
    ASSERT_EQ(services.config.settings().selected_flash_transport, flash_transport);
    ASSERT_EQ(services.config.settings().selected_log_transport, log_transport);

    // Saved: a fresh session over the same root restores row 1.
    QtEventSink reread_events;
    fastecu::config::ConfigSession reread{services.file_system, services.resource_bundle, services.file_repository,
                                          reread_events};
    ASSERT_TRUE(reread.initialize(config_root_->path().toStdString(), kTestApplication.version).has_value());
    ASSERT_EQ(reread.settings().selected_protocol_id, std::string("1"));
}

TEST_F(MainWindowTest, acceptedVehicleChoiceSelectsTheRowAndSavesIt)
{
    ASSERT_NO_FATAL_FAILURE(check_acceptedVehicleChoiceSelectsTheRowAndSavesIt());
}

void MainWindowTest::check_cancelledVehicleChoiceChangesNothing()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    const auto before = services.config.settings();

    window.apply_vehicle_choice(QDialog::Rejected, 1);

    ASSERT_TRUE(services.config.settings() == before);
}

TEST_F(MainWindowTest, cancelledVehicleChoiceChangesNothing)
{
    ASSERT_NO_FATAL_FAILURE(check_cancelledVehicleChoiceChangesNothing());
}

void MainWindowTest::check_acceptedProtocolChoiceSelectsTheLastMatchingRow()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    const auto vehicles = services.config.vehicles();
    std::size_t last = 0;
    for (std::size_t i = 0; i < vehicles.size(); ++i)
    {
        if (vehicles[i].protocol_name == "sub_ecu_denso_sh7058")
        {
            last = i;
        }
    }

    window.apply_protocol_choice(QDialog::Accepted, std::string("sub_ecu_denso_sh7058"));

    ASSERT_EQ(*services.config.selected_row(), last);
}

TEST_F(MainWindowTest, acceptedProtocolChoiceSelectsTheLastMatchingRow)
{
    ASSERT_NO_FATAL_FAILURE(check_acceptedProtocolChoiceSelectsTheLastMatchingRow());
}

void MainWindowTest::check_romFlashMethodSelectsTheLastMatchingRow()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    window.update_protocol_info("sub_ecu_denso_sh7058");

    ASSERT_EQ(*services.config.selected_row(), std::size_t{9}); // rows 3, 8, 9 match; the last wins
    ASSERT_EQ(services.config.selected_vehicle()->make, std::string("Nissan"));
}

TEST_F(MainWindowTest, romFlashMethodSelectsTheLastMatchingRow)
{
    ASSERT_NO_FATAL_FAILURE(check_romFlashMethodSelectsTheLastMatchingRow());
}

void MainWindowTest::check_unmatchedRomFlashMethodChangesNothing()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    const auto before = services.config.settings();
    window.update_protocol_info("no_such_protocol");

    ASSERT_TRUE(services.config.settings() == before);
}

TEST_F(MainWindowTest, unmatchedRomFlashMethodChangesNothing)
{
    ASSERT_NO_FATAL_FAILURE(check_unmatchedRomFlashMethodChangesNothing());
}

void MainWindowTest::check_restoreLoggingUiStateUnchecksLogging()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QAction *logging = menuAction(window, kToggleRealtime);
    ASSERT_NE(logging, nullptr);
    ASSERT_TRUE(logging->isCheckable());
    logging->setChecked(true);
    window.logging_state = true;

    window.restoreLoggingUiState();

    EXPECT_FALSE(logging->isChecked());
    EXPECT_FALSE(window.logging_state);
}

TEST_F(MainWindowTest, restoreLoggingUiStateUnchecksLogging)
{
    ASSERT_NO_FATAL_FAILURE(check_restoreLoggingUiStateUnchecksLogging());
}

void MainWindowTest::check_setRealtimeStateChecksAndUnchecksLogging()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QAction *logging = menuAction(window, kToggleRealtime);
    ASSERT_NE(logging, nullptr);

    window.set_realtime_state(true);
    EXPECT_TRUE(logging->isChecked());
    window.set_realtime_state(false);
    EXPECT_FALSE(logging->isChecked());
}

TEST_F(MainWindowTest, setRealtimeStateChecksAndUnchecksLogging)
{
    ASSERT_NO_FATAL_FAILURE(check_setRealtimeStateChecksAndUnchecksLogging());
}

void MainWindowTest::check_identificationDisablesLoggingAndConnectButNotDisconnect()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QAction *logging = menuAction(window, kToggleRealtime);
    QAction *connect_action = menuAction(window, kConnectToEcu);
    QAction *disconnect_action = menuAction(window, kDisconnectFromEcu);
    ASSERT_NE(logging, nullptr);
    ASSERT_NE(connect_action, nullptr);
    ASSERT_NE(disconnect_action, nullptr);

    window.connection_presentation_.set_controls_locked(true);
    EXPECT_FALSE(logging->isEnabled());
    EXPECT_FALSE(connect_action->isEnabled());
    EXPECT_TRUE(disconnect_action->isEnabled());

    window.connection_presentation_.set_controls_locked(false);
    EXPECT_TRUE(logging->isEnabled());
    EXPECT_TRUE(connect_action->isEnabled());
    EXPECT_TRUE(disconnect_action->isEnabled());
}

TEST_F(MainWindowTest, identificationDisablesLoggingAndConnectButNotDisconnect)
{
    ASSERT_NO_FATAL_FAILURE(check_identificationDisablesLoggingAndConnectButNotDisconnect());
}

void MainWindowTest::check_logToFileActionDrivesWriteDatalogToFile()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QAction *log_to_file = menuAction(window, kLogToFile);
    ASSERT_NE(log_to_file, nullptr);
    ASSERT_TRUE(log_to_file->isCheckable());

    log_to_file->setChecked(true);
    window.toggle_log_to_file();
    EXPECT_TRUE(window.write_datalog_to_file);

    log_to_file->setChecked(false);
    window.toggle_log_to_file();
    EXPECT_FALSE(window.write_datalog_to_file);
}

TEST_F(MainWindowTest, logToFileActionDrivesWriteDatalogToFile)
{
    ASSERT_NO_FATAL_FAILURE(check_logToFileActionDrivesWriteDatalogToFile());
}

void MainWindowTest::check_unresolvedProtocolRowLeavesReadAndWriteUnavailable()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    const QList<QAction *> actions{menuAction(window, kReadRomFromEcu), menuAction(window, kTestWriteRomToEcu),
                                   menuAction(window, kWriteRomToEcu)};
    for (QAction *action : actions)
    {
        ASSERT_NE(action, nullptr);
    }

    // A resolved row with every capability enables all three...
    ASSERT_NO_FATAL_FAILURE(selectProtocol(window, "sub_ecu_denso_sh7058_can"));
    window.set_flash_arrow_state();
    for (QAction *action : actions)
    {
        ASSERT_TRUE(action->isEnabled()) << qPrintable(action->text());
    }

    // ...and the unresolved row 10 (no <protocol> of that name) none.
    ASSERT_NO_FATAL_FAILURE(selectProtocol(window, "sub_ecu_orphan"));
    window.set_flash_arrow_state();
    for (QAction *action : actions)
    {
        ASSERT_TRUE(!action->isEnabled()) << qPrintable(action->text());
    }
}

TEST_F(MainWindowTest, unresolvedProtocolRowLeavesReadAndWriteUnavailable)
{
    ASSERT_NO_FATAL_FAILURE(check_unresolvedProtocolRowLeavesReadAndWriteUnavailable());
}

void MainWindowTest::check_loggingUsesTheSessionLogProtocol()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    prepareLogging(window, "CDBG");
    services.logging_engine.registerProtocol("CDBG",
                                             [](const fastecu::desktop::logging::DesktopLoggingSnapshot&)
                                             {
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->blockPollUntilCancelled();
                                                 return protocol;
                                             });
    window.configSession->settings().selected_log_protocol = "CDBG";

    ModalDriver driver{QString()};
    driver.start();
    window.continue_start_logging();
    driver.stop();
    services.logging_engine.stop();

    ASSERT_EQ(window.activeLogValueProtocolFilter, QString("CDBG"));
}

TEST_F(MainWindowTest, loggingUsesTheSessionLogProtocol)
{
    ASSERT_NO_FATAL_FAILURE(check_loggingUsesTheSessionLogProtocol());
}

void MainWindowTest::check_selectedSerialPortIsEmptyWithoutPorts()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    window.serial_ports.clear();
    window.serial_port_list->clear();
    ASSERT_EQ(window.selected_serial_port(), QString());
    window.serial_ports = {"ttyUSB0"};
    window.serial_port_list->addItem("ttyUSB0");
    ASSERT_EQ(window.selected_serial_port(), QString("ttyUSB0"));
}

TEST_F(MainWindowTest, selectedSerialPortIsEmptyWithoutPorts)
{
    ASSERT_NO_FATAL_FAILURE(check_selectedSerialPortIsEmptyWithoutPorts());
}

void MainWindowTest::check_dtcWindowWithoutAPortWarnsInsteadOfCrashing()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    window.serial_ports.clear();
    window.serial_port_list->clear();
    EXPECT_CALL(*services.fake, set_serial_port_list(::testing::_)).Times(0);
    ModalDriver driver{QString()};
    driver.start();
    for (const ActionName& command : {kDtcWindow, kBiuCommunication, kTerminal})
    {
        ASSERT_TRUE(triggerMenu(window, command));
    }
    driver.stop();
    ASSERT_TRUE(!driver.timedOut());
}

TEST_F(MainWindowTest, dtcWindowWithoutAPortWarnsInsteadOfCrashing)
{
    ASSERT_NO_FATAL_FAILURE(check_dtcWindowWithoutAPortWarnsInsteadOfCrashing());
}

void MainWindowTest::check_repeatedSaveFailuresLogOnceUntilASuccess()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    TestServices services{root.path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    fastecu::testing::SignalRecorder errors{&window, &MainWindow::LOG_E};
    const QString config_file = QString::fromStdString(services.config.provisioned_paths().config_file);
    ASSERT_TRUE(QFile::remove(config_file));
    ASSERT_TRUE(QDir().mkpath(config_file));

    services.config.settings().toolbar_iconsize = "48";
    window.save_settings();
    window.save_settings();
    window.save_settings();
    ASSERT_EQ(errors.count(), 1U);
    ASSERT_TRUE(std::get<0>(errors.snapshot().front()).contains(config_file));
    ASSERT_EQ(services.config.settings().toolbar_iconsize, std::string("48"));

    ASSERT_TRUE(QDir().rmdir(config_file));
    window.save_settings();
    ASSERT_EQ(errors.count(), 1U);
    QFile saved{config_file};
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    ASSERT_TRUE(saved.readAll().contains(R"(data="48")"));
    saved.close();
    ASSERT_TRUE(QFile::remove(config_file));
    ASSERT_TRUE(QDir().mkpath(config_file));
    window.save_settings();
    ASSERT_EQ(errors.count(), 2U);
}

TEST_F(MainWindowTest, repeatedSaveFailuresLogOnceUntilASuccess)
{
    ASSERT_NO_FATAL_FAILURE(check_repeatedSaveFailuresLogOnceUntilASuccess());
}

void MainWindowTest::check_biuWindowRemembersTheOpenedPort()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    ON_CALL(*services.fake, get_openedSerialPort()).WillByDefault(::testing::Return(QString("ttyUSB0")));
    window.previous_serial_port.clear();
    window.configSession->settings().serial_port = "none";
    window.save_settings();
    const QString config_file =
        config_root_->path() + "/" + QString::fromStdString(kTestApplication.version) + "/config/fastecu.cfg";

    ModalDriver driver{QString()};
    driver.start();
    ASSERT_TRUE(triggerMenu(window, kBiuCommunication));
    driver.stop();

    // As open_serial_port did for the legacy BIU path: the chosen port is
    // remembered for the next launch and as the previously opened port.
    ASSERT_EQ(window.previous_serial_port, QString("ttyUSB0"));
    ASSERT_EQ(window.configSession->settings().serial_port, std::string("ttyUSB0"));
    QFile saved{config_file};
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    ASSERT_TRUE(saved.readAll().contains(R"(data="ttyUSB0")"));
}

TEST_F(MainWindowTest, biuWindowRemembersTheOpenedPort)
{
    ASSERT_NO_FATAL_FAILURE(check_biuWindowRemembersTheOpenedPort());
}

void MainWindowTest::check_disconnectReturnsTheAdapterToIdle()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    {
        ::testing::InSequence order;
        EXPECT_CALL(*services.fake, reset_connection());
        EXPECT_CALL(*services.fake, set_serial_port_baudrate(QString("4800")));
        EXPECT_CALL(*services.fake, set_serial_port_parity(0));
    }
    EXPECT_CALL(*services.fake, set_is_can_connection(::testing::_)).Times(0);
    ASSERT_TRUE(triggerMenu(window, kDisconnectFromEcu));
    // Check now, so facade teardown cannot over-saturate the expectations.
    ASSERT_TRUE(::testing::Mock::VerifyAndClearExpectations(services.fake));
    ASSERT_TRUE(window.serial_port_list->isEnabled());
}

TEST_F(MainWindowTest, disconnectReturnsTheAdapterToIdle)
{
    ASSERT_NO_FATAL_FAILURE(check_disconnectReturnsTheAdapterToIdle());
}

void MainWindowTest::check_connectOnAnotherMakeDisconnectsWithoutIdentifying()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Mitsubishi", "K-Line"));
    EXPECT_CALL(*services.fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(*services.fake, set_serial_port_parity(0)).Times(::testing::AtLeast(1));

    QElapsedTimer elapsed;
    elapsed.start();
    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(elapsed.elapsed() < 1000); // the legacy loop waited 2.5 s here
    ASSERT_TRUE(!window.connection_coordinator_->identifying());
    ASSERT_TRUE(!window.ecu_init_complete);
    ASSERT_TRUE(window.serial_port_list->isEnabled());
}

TEST_F(MainWindowTest, connectOnAnotherMakeDisconnectsWithoutIdentifying)
{
    ASSERT_NO_FATAL_FAILURE(check_connectOnAnotherMakeDisconnectsWithoutIdentifying());
}

void MainWindowTest::check_subaruKlineConnectIdentifiesOffTheUiThread()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    std::atomic<bool> read_off_ui_thread = false;
    EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
        .WillOnce(::testing::Invoke(
            [&window, &read_off_ui_thread](std::uint16_t)
            {
                read_off_ui_thread.store(QThread::currentThread() != window.thread());
                return kEcuInit;
            }))
        .WillRepeatedly(::testing::Return(QByteArray{}));

    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_TRUE(!window.log_transport_list->isEnabled());
    ASSERT_TRUE(!window.serial_port_list->isEnabled());

    constructor_driver.start();
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return !window.connection_coordinator_->identifying(); },
                                             std::chrono::milliseconds(5000)));
    constructor_driver.stop();
    ASSERT_TRUE(window.ecu_init_complete);
    ASSERT_EQ(window.ecuid, QString("3152584006"));
    ASSERT_TRUE(read_off_ui_thread.load());
    ASSERT_TRUE(!window.connection_coordinator_->identifying());
    ASSERT_TRUE(window.log_transport_list->isEnabled());
    ASSERT_TRUE(!window.serial_port_list->isEnabled()); // stays locked while connected, as before
}

TEST_F(MainWindowTest, subaruKlineConnectIdentifiesOffTheUiThread)
{
    ASSERT_NO_FATAL_FAILURE(check_subaruKlineConnectIdentifiesOffTheUiThread());
}

void MainWindowTest::check_subaruConnectThatNeverAnswersDisconnectsAndRestoresControls()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));

    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return !window.connection_coordinator_->identifying(); },
                                             std::chrono::milliseconds(15000)));
    ASSERT_TRUE(!window.ecu_init_complete);
    ASSERT_TRUE(window.log_transport_list->isEnabled());
    ASSERT_TRUE(window.serial_port_list->isEnabled());
    ASSERT_TRUE(window.ecu_radio_button->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button->isEnabled());
}

TEST_F(MainWindowTest, subaruConnectThatNeverAnswersDisconnectsAndRestoresControls)
{
    ASSERT_NO_FATAL_FAILURE(check_subaruConnectThatNeverAnswersDisconnectsAndRestoresControls());
}

void MainWindowTest::check_disconnectDuringIdentificationCancelsAndDropsTheResult()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));

    EXPECT_CALL(*services.fake, read_serial_data(::testing::_)).WillOnce(::testing::Return(kEcuInit));
    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    // Leave a successful completion queued on the UI thread before cancelling.
    ASSERT_TRUE(window.identify_launcher_->wait_for_worker(std::chrono::milliseconds(5000)));
    ASSERT_TRUE(triggerMenu(window, kDisconnectFromEcu));
    ASSERT_TRUE(!window.connection_coordinator_->identifying());
    ASSERT_TRUE(window.log_transport_list->isEnabled());
    ASSERT_TRUE(window.serial_port_list->isEnabled());
    ASSERT_TRUE(window.ecu_radio_button->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button->isEnabled());
    fastecu::testing::process_events_for(
        std::chrono::milliseconds(200)); // any completion already queued must be dropped
    ASSERT_TRUE(!window.ecu_init_complete);
}

TEST_F(MainWindowTest, disconnectDuringIdentificationCancelsAndDropsTheResult)
{
    ASSERT_NO_FATAL_FAILURE(check_disconnectDuringIdentificationCancelsAndDropsTheResult());
}

void MainWindowTest::check_loggingSelectionFailureSemanticsAndSupportPreservation()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    window.vbatt_timer->stop();
    const auto cfg = QString::fromStdString(services.config.effective_paths().logger_file);
    window.ecuid = "MODEL_TEST";
    installLoggingFixture(
        window,
        {.parameters = {{.protocol = "SSM", .id = "rpm", .ecu_byte_index = "0", .ecu_bit = "0", .enabled = true}},
         .switches = {{.protocol = "SSM", .id = "flag", .ecu_byte_index = "5", .enabled = true}}},
        {.protocol = "SSM", .gauge_ids = {"old"}, .lower_panel_ids = {"old"}, .switch_ids = {"old"}});
    const auto previous = window.loggerModel->selection();
    ASSERT_TRUE(QFile::remove(cfg));
    window.load_logger_selection();
    ASSERT_TRUE(window.loggerModel->selection() == previous);
    window.loggerModel->set_selection({.protocol = "SSM", .gauge_ids = {"operator-edit", "unresolved"}});
    const auto edited = window.loggerModel->selection();
    window.save_logger_selection();
    ASSERT_TRUE(window.loggerModel->selection() == edited);
    ASSERT_TRUE(writeTextFile(cfg, "<config><logger/></config>"));
    window.loggerModel->set_parameter_supported("SSM", "rpm", false);
    window.load_logger_selection();
    ASSERT_TRUE(window.loggerModel->selection().gauge_ids.empty());
    ASSERT_EQ(window.loggerModel->selection().switch_ids, (std::vector<std::string>{"flag"}));
    ASSERT_TRUE(!window.loggerModel->parameter_supported("SSM", "rpm"));
    ASSERT_TRUE(writeTextFile(
        cfg,
        R"(<config><logger><ecu id="MODEL_TEST"><protocol id="SSM"><parameters><gauges><parameter id="unknown"/></gauges><lower_panel><parameter id="rpm"/></lower_panel></parameters><switches><switch id="flag"/></switches></protocol></ecu></logger></config>)"));
    window.load_logger_selection();
    ASSERT_EQ(window.loggerModel->selection().gauge_ids, (std::vector<std::string>{"unknown"}));
    ASSERT_TRUE(!window.loggerModel->parameter_supported("SSM", "rpm"));
    // A valid capability byte updates parameters; missing switch bytes retain flags.
    window.parse_log_value_list(frame({0, 0, 0, 0, 0, 1}), "SSM");
    ASSERT_TRUE(window.loggerModel->parameter_supported("SSM", "rpm"));
    ASSERT_TRUE(window.loggerModel->switch_supported("SSM", "flag"));
    ASSERT_TRUE(window.loggerModel->definition().parameters.front().enabled);
    window.save_logger_selection();
    const auto stored = services.logger_definitions.load_selection(cfg.toStdString(), "MODEL_TEST");
    ASSERT_TRUE(stored.has_value());
    ASSERT_TRUE(stored->has_value());
    ASSERT_TRUE(**stored == window.loggerModel->selection());
    // Missing definitions clear stale IDs after a successful read and never persist defaults.
    installLoggingFixture(window, {}, {.protocol = "SSM", .lower_panel_ids = {"stale"}});
    ASSERT_TRUE(writeTextFile(cfg, "<config><logger/></config>"));
    window.load_logger_selection();
    ASSERT_TRUE(window.loggerModel->selection().lower_panel_ids.empty());
    QFile conf{cfg};
    ASSERT_TRUE(conf.open(QIODevice::ReadOnly));
    ASSERT_EQ(conf.readAll(), QByteArray("<config><logger/></config>"));
    driver.stop();
}

TEST_F(MainWindowTest, loggingSelectionFailureSemanticsAndSupportPreservation)
{
    ASSERT_NO_FATAL_FAILURE(check_loggingSelectionFailureSemanticsAndSupportPreservation());
}

void MainWindowTest::check_loggingDefinitionFailureIsNonfatal()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    services.config.settings().romraider_logger_definition_file = "/missing/logger.xml";
    MainWindow window{services.services()};
    ASSERT_TRUE(window.loggerModel->definition().parameters.empty());
    ASSERT_TRUE(window.loggerModel->selection().lower_panel_ids.empty());
    ASSERT_TRUE(window.ui != nullptr);
    driver.stop();
}

TEST_F(MainWindowTest, loggingDefinitionFailureIsNonfatal)
{
    ASSERT_NO_FATAL_FAILURE(check_loggingDefinitionFailureIsNonfatal());
}

void MainWindowTest::check_unresolvedDisplaySlotsAreSkippedAndUpdateTheirOriginalLabels()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    driver.stop();
    installLoggingFixture(window,
                          {.parameters = {{.protocol = "SSM",
                                           .id = "rpm",
                                           .name = "Speed",
                                           .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}}}},
                          {.protocol = "SSM", .lower_panel_ids = {"unresolved", "rpm", "another-unresolved"}});
    window.update_logboxes("SSM");
    ASSERT_EQ(window.ui->logBoxLayout->count(), 1);
    ASSERT_TRUE(window.loggerValues.set_parameter_value({"SSM", "rpm"}, "123.00"));
    window.update_logbox_values("SSM");
    const auto *label = window.findChild<QLabel *>("log_label1");
    ASSERT_TRUE(label != nullptr);
    ASSERT_EQ(label->text(), QString("123.00 <font size=1px color=grey>rpm</font>"));
    ASSERT_EQ(label->alignment(), Qt::Alignment(Qt::AlignRight));
    ASSERT_EQ(label->font().pointSize(), QGuiApplication::primaryScreen()->geometry().width() / 90);
}

TEST_F(MainWindowTest, unresolvedDisplaySlotsAreSkippedAndUpdateTheirOriginalLabels)
{
    ASSERT_NO_FATAL_FAILURE(check_unresolvedDisplaySlotsAreSkippedAndUpdateTheirOriginalLabels());
}

struct chooserDuplicateLabelIdentityCase
{
    std::string name;
    int tab;
    QString kind;
};
std::vector<chooserDuplicateLabelIdentityCase> chooserDuplicateLabelIdentityRows()
{
    std::vector<chooserDuplicateLabelIdentityCase> rows;

    rows.push_back(chooserDuplicateLabelIdentityCase{"gauge", 0, QString("Gauge")});
    rows.push_back(chooserDuplicateLabelIdentityCase{"digital", 1, QString("Digital")});
    rows.push_back(chooserDuplicateLabelIdentityCase{"switch", 2, QString("Switch")});

    return rows;
}
class chooserDuplicateLabelIdentityParameters : public MainWindowTest,
                                                public ::testing::WithParamInterface<chooserDuplicateLabelIdentityCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, chooserDuplicateLabelIdentityParameters,
                         ::testing::ValuesIn(chooserDuplicateLabelIdentityRows()),
                         [](const ::testing::TestParamInfo<chooserDuplicateLabelIdentityCase>& info)
                         {
                             auto name = info.param.name;
                             for (char& c : name)
                             {
                                 if (!std::isalnum(static_cast<unsigned char>(c)))
                                 {
                                     c = '_';
                                 }
                             }
                             return name;
                         });
void MainWindowTest::check_chooserDuplicateLabelIdentity(int tab, QString kind)
{

    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    driver.stop();
    prepareLogging(window, "SSM");
    installLoggingFixture(
        window,
        {.parameters = {{.protocol = "OTHER", .id = "first", .name = "Same", .enabled = true},
                        {.protocol = "SSM", .id = "first", .name = "Same", .enabled = true},
                        {.protocol = "SSM", .id = "second", .name = "Same", .enabled = true}},
         .switches = {{.protocol = "OTHER", .id = "first", .name = "Same", .enabled = true},
                      {.protocol = "SSM", .id = "first", .name = "Same", .enabled = true},
                      {.protocol = "SSM", .id = "second", .name = "Same", .enabled = true}}},
        {.protocol = "SSM", .gauge_ids = {"second"}, .lower_panel_ids = {"second"}, .switch_ids = {"second"}});
    bool inspected = false;
    QTimer::singleShot(0, &window,
                       [&]
                       {
                           auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                           ASSERT_TRUE(dialog != nullptr);
                           auto *combo = dialog->findChild<QComboBox *>(kind + " value 0");
                           ASSERT_TRUE(combo != nullptr);
                           ASSERT_EQ(combo->count(), 2);
                           ASSERT_EQ(combo->currentData().toStringList(), (QStringList{"SSM", "second"}));
                           ASSERT_EQ(combo->itemText(0), QString("Same"));
                           ASSERT_EQ(combo->itemText(1), QString("Same"));
                           combo->setCurrentIndex(0);
                           inspected = true;
                           dialog->reject();
                       });
    window.change_log_values(tab, "SSM");
    ASSERT_TRUE(inspected);
    const auto& selected = window.loggerModel->selection();
    ASSERT_EQ((tab == 0   ? selected.gauge_ids
               : tab == 1 ? selected.lower_panel_ids
                          : selected.switch_ids)
                  .at(0),
              std::string("first"));
    ASSERT_EQ(selected.protocol, std::string("SSM"));
}

TEST_P(chooserDuplicateLabelIdentityParameters, chooserDuplicateLabelIdentity)
{
    ASSERT_NO_FATAL_FAILURE(check_chooserDuplicateLabelIdentity(GetParam().tab, GetParam().kind));
}

void MainWindowTest::check_csvSharedIdProtocolIdentity()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    driver.stop();
    prepareLogging(window, "CDBG");
    installLoggingFixture(window,
                          {.parameters = {{.protocol = "SSM",
                                           .id = "rpm",
                                           .name = "Wrong SSM",
                                           .address = "10",
                                           .length = "1",
                                           .enabled = true,
                                           .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}},
                                          {.protocol = "CDBG",
                                           .id = "rpm",
                                           .name = "Correct CDBG",
                                           .address = "10",
                                           .length = "1",
                                           .enabled = true,
                                           .conversions = {{"rpm", "x", "0.00", "0", "100", "1"}}}}},
                          {.protocol = "CDBG",
                           .gauge_ids = {"missing"},
                           .lower_panel_ids = {"rpm", "unresolved"},
                           .switch_ids = {"missing-switch"}});
    ASSERT_TRUE(window.loggerValues.set_parameter_value({"SSM", "rpm"}, "11.00"));
    ASSERT_TRUE(window.loggerValues.set_parameter_value({"CDBG", "rpm"}, "22.00"));
    auto snapshot = fastecu::desktop::logging::make_desktop_logging_snapshot(
        *window.loggerModel, fastecu::logging::LoggingProtocolId::Cdbg, "CDBG",
        {.poll_timeout = std::chrono::milliseconds{50},
         .car_silence_miss_threshold = 20,
         .reconnect_attempt_threshold = 100,
         .reconnect_retry_period = 20});
    ASSERT_TRUE(snapshot.has_value());
    window.activeLoggingSnapshot = *snapshot;
    window.protocol = "SSM"; // active run, not mutable UI choice, owns CSV protocol
    ASSERT_TRUE(QDir().mkpath(QString::fromStdString(services.config.effective_paths().datalog_files_directory)));
    window.write_datalog_to_file = true;
    window.log_to_file();
    window.log_to_file();
    window.datalog_file_outstream.flush();
    QFile csv{window.datalog_file.fileName()};
    ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
    const auto content = csv.readAll();
    ASSERT_TRUE(content.startsWith("Time,,Correct CDBG,,,\n"));
    ASSERT_TRUE(content.contains(",,22.00,,,\n"));
    ASSERT_TRUE(!content.contains("Wrong SSM"));
    ASSERT_TRUE(!content.contains("11.00"));
    window.datalog_file.close();
}

TEST_F(MainWindowTest, csvSharedIdProtocolIdentity)
{
    ASSERT_NO_FATAL_FAILURE(check_csvSharedIdProtocolIdentity());
}

struct loggingStartWaitsForIdentificationCase
{
    std::string name;
    bool target_is_ecu;
};
std::vector<loggingStartWaitsForIdentificationCase> loggingStartWaitsForIdentificationRows()
{
    std::vector<loggingStartWaitsForIdentificationCase> rows;

    rows.push_back(loggingStartWaitsForIdentificationCase{"ECU", true});
    rows.push_back(loggingStartWaitsForIdentificationCase{"TCU", false});

    return rows;
}
class loggingStartWaitsForIdentificationParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<loggingStartWaitsForIdentificationCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, loggingStartWaitsForIdentificationParameters,
                         ::testing::ValuesIn(loggingStartWaitsForIdentificationRows()),
                         [](const ::testing::TestParamInfo<loggingStartWaitsForIdentificationCase>& info)
                         {
                             auto name = info.param.name;
                             for (char& c : name)
                             {
                                 if (!std::isalnum(static_cast<unsigned char>(c)))
                                 {
                                     c = '_';
                                 }
                             }
                             return name;
                         });
void MainWindowTest::check_loggingStartWaitsForIdentification(bool target_is_ecu)
{

    QSemaphore response_gate;
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "iso15765"));
    window.protocol = "SSM";
    (target_is_ecu ? window.ecu_radio_button : window.tcu_radio_button)->setChecked(true);
    EXPECT_CALL(*services.fake,
                write_serial_data_echo_check(frame({0x00, 0x00, 0x07, target_is_ecu ? 0xE0 : 0xE1, 0x22, 0xF1, 0x82})));
    EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
        .WillOnce(::testing::Invoke(
            [&response_gate](std::uint16_t)
            {
                // Bound the wait so an assertion failure can still join the worker.
                response_gate.tryAcquire(1, 5000);
                return frame({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82, 0x12, 0x34, 0x56, 0x78, 0x9A});
            }))
        .WillRepeatedly(::testing::Return(QByteArray{}));
    QAction *action = menuAction(window, kToggleRealtime);
    installLoggingFixture(window,
                          {.parameters = {{.protocol = std::string("SSM"),
                                           .id = "rpm",
                                           .name = "rpm",
                                           .address = "000010",
                                           .length = "1",
                                           .ecu_byte_index = "0",
                                           .ecu_bit = "0",
                                           .target = "ECU",
                                           .enabled = true,
                                           .conversions = {{"rpm", "x", "0", "0", "100", "1"}}}}},
                          {.protocol = std::string("SSM"), .lower_panel_ids = {"rpm"}});
    bool target_frozen_in_continuation = false;
    services.logging_engine.registerProtocol(
        "SSM",
        [&window, &target_frozen_in_continuation](const fastecu::desktop::logging::DesktopLoggingSnapshot&)
        {
            target_frozen_in_continuation =
                !window.ecu_radio_button->isEnabled() && !window.tcu_radio_button->isEnabled();
            auto protocol = std::make_unique<ScriptedLoggingProtocol>();
            protocol->blockPollUntilCancelled();
            return protocol;
        });

    // trigger() toggles a checkable action, as a click does: start unchecked
    // so the handler sees Logging switched on.
    action->setChecked(false);
    ASSERT_TRUE(triggerMenu(window, kToggleRealtime));
    ASSERT_TRUE(action->isChecked());
    ASSERT_TRUE(!window.activeLoggingSnapshot.has_value()); // still identifying
    (target_is_ecu ? window.tcu_radio_button : window.ecu_radio_button)->click();
    response_gate.release();
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return window.activeLoggingSnapshot.has_value(); },
                                             std::chrono::milliseconds(5000)));
    ASSERT_EQ(window.ecuid, QString("123456789A"));
    ASSERT_TRUE(window.loggerModel->parameter_supported("SSM", "rpm"));
    ASSERT_EQ(window.activeLoggingSnapshot->target_is_ecu, target_is_ecu);
    ASSERT_TRUE(target_frozen_in_continuation);
    ASSERT_TRUE(window.ecu_radio_button->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button->isEnabled());
    services.logging_engine.stop();
}

TEST_P(loggingStartWaitsForIdentificationParameters, loggingStartWaitsForIdentification)
{
    ASSERT_NO_FATAL_FAILURE(check_loggingStartWaitsForIdentification(GetParam().target_is_ecu));
}

void MainWindowTest::check_batterySamplingDoesNotUseTheFacadeDuringIdentification()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    EXPECT_CALL(*services.fake, get_use_openport2_adapter()).Times(0);
    window.update_vbatt();
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_TRUE(::testing::Mock::VerifyAndClearExpectations(services.fake));
}

TEST_F(MainWindowTest, batterySamplingDoesNotUseTheFacadeDuringIdentification)
{
    ASSERT_NO_FATAL_FAILURE(check_batterySamplingDoesNotUseTheFacadeDuringIdentification());
}

void MainWindowTest::check_windowDestructionJoinsIdentificationWithoutContinuingLogging()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    auto window = std::make_unique<MainWindow>(services.services());
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(*window, *services.fake, "Subaru", "K-Line"));
    bool continued = false;
    window->connect_to_ecu([&continued](bool) { continued = true; });
    ASSERT_TRUE(window->connection_coordinator_->identifying());
    window.reset();
    fastecu::testing::process_events_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(!continued);
}

TEST_F(MainWindowTest, windowDestructionJoinsIdentificationWithoutContinuingLogging)
{
    ASSERT_NO_FATAL_FAILURE(check_windowDestructionJoinsIdentificationWithoutContinuingLogging());
}

void MainWindowTest::check_connectStopsAnActiveLoggingWorkerBeforeIdentification()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    services.logging_engine.registerProtocol("SSM",
                                             [](const fastecu::desktop::logging::DesktopLoggingSnapshot&)
                                             {
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->blockPollUntilCancelled();
                                                 return protocol;
                                             });
    auto session =
        fastecu::logging::make_logging_session(fastecu::logging::LoggingProtocolId::Ssm,
                                               {{.id = "rpm",
                                                 .address = 0x10,
                                                 .length = 1,
                                                 .raw_assembly = fastecu::logging::RawAssembly::UnsignedIntegerDecimal,
                                                 .from_byte_expression = "x",
                                                 .unit = "rpm",
                                                 .decimal_precision = 0}},
                                               {.poll_timeout = std::chrono::milliseconds{50},
                                                .car_silence_miss_threshold = 20,
                                                .reconnect_attempt_threshold = 100,
                                                .reconnect_retry_period = 20});
    ASSERT_TRUE(session.has_value());
    fastecu::desktop::logging::DesktopLoggingSnapshot snapshot{.session = std::move(*session)};
    ASSERT_TRUE(services.logging_engine.start({"SSM"}, std::move(snapshot)).has_value());
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return services.logging_engine.isRunning(); },
                                             std::chrono::milliseconds(5000)));
    window.logging_state = true;
    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_TRUE(!services.logging_engine.isRunning());
    ASSERT_TRUE(!window.logging_state);
}

TEST_F(MainWindowTest, connectStopsAnActiveLoggingWorkerBeforeIdentification)
{
    ASSERT_NO_FATAL_FAILURE(check_connectStopsAnActiveLoggingWorkerBeforeIdentification());
}

struct connectionEntryPointsStopIdentificationCase
{
    std::string name;
    QString entry_point;
};
std::vector<connectionEntryPointsStopIdentificationCase> connectionEntryPointsStopIdentificationRows()
{
    std::vector<connectionEntryPointsStopIdentificationCase> rows;

    for (const char *name : {"log_transport_changed", "check_serial_ports", "open_serial_port", "show_dtc_window",
                             "show_subaru_biu_window", "show_terminal_window"})
    {
        rows.push_back(connectionEntryPointsStopIdentificationCase{name, QString::fromLatin1(name)});
    }

    return rows;
}
class connectionEntryPointsStopIdentificationParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<connectionEntryPointsStopIdentificationCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, connectionEntryPointsStopIdentificationParameters,
                         ::testing::ValuesIn(connectionEntryPointsStopIdentificationRows()),
                         [](const ::testing::TestParamInfo<connectionEntryPointsStopIdentificationCase>& info)
                         {
                             auto name = info.param.name;
                             for (char& c : name)
                             {
                                 if (!std::isalnum(static_cast<unsigned char>(c)))
                                 {
                                     c = '_';
                                 }
                             }
                             return name;
                         });
void MainWindowTest::check_connectionEntryPointsStopIdentification(QString entry_point)
{

    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    bool cancelled = false;
    window.connect_to_ecu([&cancelled](bool connected) { cancelled = !connected; });
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_TRUE(!window.serial_port_list->isEnabled());
    ASSERT_TRUE(!window.refresh_serial_port_list->isEnabled());
    QTimer close_dialog;
    close_dialog.setInterval(5);
    QObject::connect(&close_dialog, &QTimer::timeout,
                     []
                     {
                         for (QWidget *widget : QApplication::topLevelWidgets())
                         {
                             if (auto *dialog = qobject_cast<QDialog *>(widget); dialog && dialog->isVisible())
                             {
                                 dialog->reject();
                             }
                         }
                     });
    close_dialog.start();
    if (entry_point == "show_dtc_window")
    {
        window.show_dtc_window();
    }
    else if (entry_point == "show_subaru_biu_window")
    {
        window.show_subaru_biu_window();
    }
    else if (entry_point == "show_terminal_window")
    {
        window.show_terminal_window();
    }
    else
    {
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, entry_point.toLatin1().constData(), Qt::DirectConnection));
    }
    ASSERT_TRUE(!window.connection_coordinator_->identifying());
    ASSERT_TRUE(cancelled);
    // A cancelled identification leaves no ECU connected, so the port
    // selector unlocks as it does after Disconnect.
    ASSERT_TRUE(window.serial_port_list->isEnabled());
    ASSERT_TRUE(window.refresh_serial_port_list->isEnabled());
    fastecu::testing::process_events_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(!window.ecu_init_complete);
}

TEST_P(connectionEntryPointsStopIdentificationParameters, connectionEntryPointsStopIdentification)
{
    ASSERT_NO_FATAL_FAILURE(check_connectionEntryPointsStopIdentification(GetParam().entry_point));
}

void MainWindowTest::check_nestedConnectDuringCapabilityNoticeKeepsEachContinuation()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
        .WillOnce(::testing::Return(kEcuInit))
        .WillRepeatedly(::testing::Return(QByteArray{}));
    std::optional<bool> first_result;
    std::optional<bool> second_result;
    window.connect_to_ecu([&first_result](bool connected) { first_result = connected; });
    ASSERT_TRUE(window.identify_launcher_->wait_for_worker(std::chrono::milliseconds(5000)));
    bool restarted = false;
    bool target_frozen_in_notice = false;
    QTimer notice_driver;
    notice_driver.setInterval(5);
    QObject::connect(&notice_driver, &QTimer::timeout,
                     [&]
                     {
                         for (QWidget *widget : QApplication::topLevelWidgets())
                         {
                             if (auto *notice = qobject_cast<QMessageBox *>(widget); notice && notice->isVisible())
                             {
                                 if (!restarted)
                                 {
                                     target_frozen_in_notice =
                                         !window.ecu_radio_button->isEnabled() && !window.tcu_radio_button->isEnabled();
                                     restarted = true;
                                     window.connect_to_ecu([&second_result](bool connected)
                                                           { second_result = connected; });
                                 }
                                 notice->accept();
                             }
                         }
                     });
    notice_driver.start();
    QCoreApplication::processEvents();
    notice_driver.stop();
    ASSERT_TRUE(restarted);
    ASSERT_TRUE(target_frozen_in_notice);
    ASSERT_TRUE(!window.ecu_radio_button->isEnabled());
    ASSERT_TRUE(!window.tcu_radio_button->isEnabled());
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_TRUE(first_result.has_value());
    ASSERT_TRUE(!*first_result);
    ASSERT_TRUE(!second_result.has_value());
    window.connection_coordinator_->cancel();
    ASSERT_TRUE(second_result.has_value());
    ASSERT_TRUE(!*second_result);
    ASSERT_TRUE(window.ecu_radio_button->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button->isEnabled());
}

TEST_F(MainWindowTest, nestedConnectDuringCapabilityNoticeKeepsEachContinuation)
{
    ASSERT_NO_FATAL_FAILURE(check_nestedConnectDuringCapabilityNoticeKeepsEachContinuation());
}

// The golden pins the menu declared in mainwindow.ui. Against the menu that
// was built at runtime from menu.cfg, it differs on purpose only in:
// - tooltips: the "<name>\n\n" prefix is dropped, and an empty tooltip falls
//   back to the action text;
// - Diagnostic Trouble Codes gains an icon (its menu.cfg path never resolved);
// - Open, Save, Save As, Copy, Paste and Quit take Qt's standard keys, with
//   literal fallbacks where a platform has none (the golden records the
//   portable text, e.g. "Ctrl+O");
// - Settings' shortcut="false", which bound nothing, is not carried over.
void MainWindowTest::check_menuMatchesTheGolden()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    const std::string actual = fastecu::ui::testing::menu_snapshot(*window.ui->menubar, *window.ui->toolBar);
    const char *golden_path = std::getenv("MAIN_MENU_GOLDEN_PATH");
    ASSERT_NE(golden_path, nullptr) << "MAIN_MENU_GOLDEN_PATH must be set by the Bazel target's env";
    std::ifstream file(golden_path, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    const std::string expected((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(actual, expected) << "actual snapshot:\n" << actual;
}

TEST_F(MainWindowTest, menuMatchesTheGolden)
{
    ASSERT_NO_FATAL_FAILURE(check_menuMatchesTheGolden());
}

void MainWindowTest::check_everyIconNamedByTheMenuResolves()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    int without_icon = 0;
    for (const QAction *action : window.findChildren<QAction *>())
    {
        if (!action->objectName().startsWith(QStringLiteral("action")))
        {
            continue;
        }
        without_icon += action->icon().isNull() ? 1 : 0;
    }
    // Set value, Interpolate bidirectional, Log views, Hex Editor, Terminal,
    // BIU communication, Get Encryption Key and WinOLS CSV have no icon by
    // design; any other null icon is a mistyped path.
    EXPECT_EQ(without_icon, 8);
}

TEST_F(MainWindowTest, everyIconNamedByTheMenuResolves)
{
    ASSERT_NO_FATAL_FAILURE(check_everyIconNamedByTheMenuResolves());
}

void MainWindowTest::check_noTwoActionsShareAShortcutAndNoneLostItsBinding()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QSet<QString> seen;
    for (const QAction *action : window.findChildren<QAction *>())
    {
        for (const QKeySequence& sequence : action->shortcuts())
        {
            const QString text = sequence.toString(QKeySequence::PortableText);
            EXPECT_FALSE(seen.contains(text)) << qPrintable(text) << " bound twice";
            seen.insert(text);
        }
    }
    for (const ActionName *name : {&kLogToFile, &kConnectToEcu, &kDisconnectFromEcu, &kToggleRealtime})
    {
        EXPECT_FALSE(menuAction(window, *name)->shortcuts().isEmpty()) << name->member;
    }
    // Open, Save, Save As, Copy, Paste and Quit had shortcuts in menu.cfg. Qt
    // has no standard key for Save As or Quit on Windows, so each must still
    // be bound there through its fallback.
    for (const char *member : {"actionOpenCalibration", "actionSaveCalibration", "actionSaveCalibrationAs",
                               "actionCopy", "actionPaste", "actionQuit"})
    {
        const auto *action = window.findChild<QAction *>(QString::fromLatin1(member));
        ASSERT_NE(action, nullptr) << member;
        EXPECT_FALSE(action->shortcut().isEmpty()) << member;
    }
}

TEST_F(MainWindowTest, noTwoActionsShareAShortcutAndNoneLostItsBinding)
{
    ASSERT_NO_FATAL_FAILURE(check_noTwoActionsShareAShortcutAndNoneLostItsBinding());
}

void MainWindowTest::check_toolbarKeepsMenuActionsBeforeTheTransportWidgets()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    const QList<QAction *> actions = window.ui->toolBar->actions();
    // Open, Save, |, Logging, Log to file, Read, Test write, Write, |, then widgets.
    ASSERT_GE(actions.size(), 10U);
    EXPECT_TRUE(actions[2]->isSeparator());
    EXPECT_TRUE(actions[8]->isSeparator());
    // widgetForAction cannot tell these apart (every action has a tool button,
    // separators have a separator widget); only a QWidgetAction hosts a widget
    // that MainWindow added in code.
    for (int i = 0; i < 9; ++i)
    {
        EXPECT_EQ(qobject_cast<QWidgetAction *>(actions[i]), nullptr) << i;
    }
    EXPECT_NE(qobject_cast<QWidgetAction *>(actions[9]), nullptr);
}

TEST_F(MainWindowTest, toolbarKeepsMenuActionsBeforeTheTransportWidgets)
{
    ASSERT_NO_FATAL_FAILURE(check_toolbarKeepsMenuActionsBeforeTheTransportWidgets());
}

void MainWindowTest::check_aStaleOrMalformedMenuCfgIsIgnored()
{
    // A private root, so the garbage file never reaches the shared fixture.
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_NO_FATAL_FAILURE(copyFixtureConfig(root.path()));
    const QString menu_cfg = root.path() + "/" + QString::fromStdString(kTestApplication.version) + "/config/menu.cfg";
    ASSERT_TRUE(writeTextFile(menu_cfg, "<<< not xml >>>"));

    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{root.path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    // The runtime menu loader raised this warning for an unreadable menu.cfg;
    // ModalDriver accepts any box it does not recognise, so check its record.
    for (const QString& text : constructor_driver.acceptedTexts())
    {
        EXPECT_FALSE(text.startsWith(QStringLiteral("Unable to load menu config file"))) << qPrintable(text);
    }
    EXPECT_NE(window.ui->actionToggleRealtime, nullptr);
    EXPECT_EQ(window.ui->menubar->findChildren<QMenu *>().size(), 7);
}

TEST_F(MainWindowTest, aStaleOrMalformedMenuCfgIsIgnored)
{
    ASSERT_NO_FATAL_FAILURE(check_aStaleOrMalformedMenuCfgIsIgnored());
}

void MainWindowTest::check_everyMenuActionIsConnectedToTheWindow()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    // QObject::isSignalConnected and receivers() are protected, so probe with
    // the public disconnect: it reports whether anything had connected the
    // action's triggered signal to the window (the context of every handler
    // lambda). It also breaks those connections, so nothing triggers the
    // actions afterwards. Which handler each one reaches is pinned by the
    // trigger-driven tests (Logging, Log to file, Connect, Disconnect, the
    // Testing windows, the Tune actions).
    int probed = 0;
    QStringList unconnected;
    for (QAction *action : window.findChildren<QAction *>())
    {
        if (!action->objectName().startsWith(QStringLiteral("action")))
        {
            continue;
        }
        ++probed;
        if (!QObject::disconnect(action, &QAction::triggered, &window, nullptr))
        {
            unconnected << action->objectName();
        }
    }
    EXPECT_EQ(probed, 31);
    EXPECT_TRUE(unconnected.isEmpty()) << "not connected: " << qPrintable(unconnected.join(", "));
}

TEST_F(MainWindowTest, everyMenuActionIsConnectedToTheWindow)
{
    ASSERT_NO_FATAL_FAILURE(check_everyMenuActionIsConnectedToTheWindow());
}

void MainWindowTest::check_triggeringLogToFileReachesItsHandler()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QAction *log_to_file = menuAction(window, kLogToFile);
    ASSERT_NE(log_to_file, nullptr);
    ASSERT_FALSE(log_to_file->isChecked());
    ASSERT_FALSE(window.write_datalog_to_file);

    // trigger() toggles the checkable action as a click does; the handler
    // then reads the new state.
    log_to_file->trigger();
    EXPECT_TRUE(window.write_datalog_to_file);
    log_to_file->trigger();
    EXPECT_FALSE(window.write_datalog_to_file);
}

TEST_F(MainWindowTest, triggeringLogToFileReachesItsHandler)
{
    ASSERT_NO_FATAL_FAILURE(check_triggeringLogToFileReachesItsHandler());
}

void MainWindowTest::check_tuneActionsEditTheSelectionThroughTheirOwnHandlers()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();
    // A handler that fails reports through a message box; record any.
    ModalDriver driver{QString()};
    driver.start();
    window.show();
    QApplication::processEvents();

    // A 3x3 uint8 map at 0x10 with identity scaling, so each cell is one ROM
    // byte. inc="10" is the coarse step; EcuFlash derives the fine step (1).
    QTemporaryDir files;
    ASSERT_TRUE(files.isValid());
    ASSERT_TRUE(writeTextFile(files.path() + "/grid.xml", R"(
<rom><romid><xmlid>GRID</xmlid></romid>
<scaling name="raw" toexpr="x" frexpr="x" format="%.0f" min="0" max="255" inc="10" storagetype="uint8" endian="big"/>
<table name="Grid" category="Tune" address="10" type="3D" sizex="3" sizey="3" scaling="raw" storagetype="uint8" endian="big">
<table type="X Axis" name="Column" address="0" elements="3" scaling="raw" storagetype="uint8" endian="big"/>
<table type="Y Axis" name="Row" address="4" elements="3" scaling="raw" storagetype="uint8" endian="big"/>
</table></rom>)"));
    services.config.settings().primary_definition_base = "ecuflash";
    services.config.settings().use_ecuflash_definitions = "enabled";
    services.config.settings().ecuflash_definition_files_directory = files.path().toStdString();
    ASSERT_TRUE(
        services.definition_catalogs.refresh_index(fastecu::definition::DefinitionFormat::EcuFlash).has_value());

    // Row-major body. The zero edges and distinct corners make each
    // interpolation direction produce a different grid.
    constexpr std::size_t kBody = 0x10;
    const bytes::Bytes body{0, 0, 20, 0, 100, 0, 40, 0, 60};
    bytes::Bytes image(64, 0);
    for (std::size_t i = 0; i < 3; ++i)
    {
        image[i] = static_cast<bytes::Byte>(i + 1);     // X axis
        image[4 + i] = static_cast<bytes::Byte>(i + 1); // Y axis
    }
    std::ranges::copy(body, image.begin() + static_cast<std::ptrdiff_t>(kBody));
    const auto opened = services.calibrations.adopt_read_image({
        .rom = image,
        .filename = "grid.bin",
        .rom_id = "GRID",
    });
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(window.add_calibration(opened->id));

    auto *file_tree = window.ui->calibrationFilesTreeWidget;
    ASSERT_EQ(file_tree->topLevelItemCount(), 1);
    file_tree->topLevelItem(0)->setSelected(true);
    window.calibration_files_treewidget_item_selected(file_tree->topLevelItem(0));
    QTreeWidget *data_tree = window.ui->calibrationDataTreeWidget;
    QTreeWidgetItem *grid_item = nullptr;
    for (int i = 0; i < data_tree->topLevelItemCount(); ++i)
    {
        if (data_tree->topLevelItem(i)->text(0) == "Tune" && data_tree->topLevelItem(i)->childCount() != 0)
        {
            grid_item = data_tree->topLevelItem(i)->child(0);
        }
    }
    ASSERT_NE(grid_item, nullptr);
    data_tree->setCurrentItem(grid_item);
    window.calibration_data_treewidget_item_selected(grid_item);
    const QList<QMdiSubWindow *> windows = window.ui->mdiArea->subWindowList();
    ASSERT_EQ(windows.size(), 1U);
    window.ui->mdiArea->setActiveSubWindow(windows.front());
    auto *table = windows.front()->findChild<QTableWidget *>(windows.front()->objectName());
    ASSERT_NE(table, nullptr);

    // Row 0 and column 0 of a 3D map's table are its axes; the body starts
    // at (1, 1).
    const auto select = [table](int top, int left, int bottom, int right)
    {
        table->clearSelection();
        table->setRangeSelected(QTableWidgetSelectionRange(top, left, bottom, right), true);
    };
    const auto grid = [&]
    {
        const bytes::ByteView rom = services.calibrations.find(opened->id)->rom();
        const auto first = rom.begin() + static_cast<std::ptrdiff_t>(kBody);
        return std::vector<int>(first, first + static_cast<std::ptrdiff_t>(body.size()));
    };

    // Chained on the centre cell (100): each step's size and sign tells the
    // four increment actions apart.
    const std::array<std::pair<QAction *, int>, 4> steps{{
        {window.ui->actionCoarseIncrement, 110},
        {window.ui->actionFineIncrement, 111},
        {window.ui->actionFineDecrement, 110},
        {window.ui->actionCoarseDecrement, 100},
    }};
    for (const auto& [action, expected] : steps)
    {
        select(2, 2, 2, 2);
        action->trigger();
        EXPECT_EQ(grid()[4], expected) << qPrintable(action->objectName());
    }

    // Each interpolation over the whole body, from the same starting grid.
    const std::array<std::pair<QAction *, std::vector<int>>, 3> interpolations{{
        {window.ui->actionInterpolateHorizontal, {0, 10, 20, 0, 0, 0, 40, 50, 60}},
        {window.ui->actionInterpolateVertical, {0, 0, 20, 20, 0, 40, 40, 0, 60}},
        {window.ui->actionInterpolateBidirectional, {0, 10, 20, 20, 30, 40, 40, 50, 60}},
    }};
    for (const auto& [action, expected] : interpolations)
    {
        ASSERT_TRUE(services.calibrations.find(opened->id)->write_bytes(kBody, body).has_value());
        select(1, 1, 3, 3);
        action->trigger();
        EXPECT_EQ(grid(), expected) << qPrintable(action->objectName());
    }

    driver.stop();
    EXPECT_TRUE(driver.acceptedTexts().isEmpty()) << qPrintable(driver.acceptedTexts().join(" | "));
    ASSERT_TRUE(!driver.timedOut());
}

TEST_F(MainWindowTest, tuneActionsEditTheSelectionThroughTheirOwnHandlers)
{
    ASSERT_NO_FATAL_FAILURE(check_tuneActionsEditTheSelectionThroughTheirOwnHandlers());
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment);
class MainWindowFixtureEnvironment : public ::testing::Environment
{
  public:
    void TearDown() override
    {
        MainWindowTest::config_root_.reset();
    }
};
const auto *const fixture_environment = ::testing::AddGlobalTestEnvironment(new MainWindowFixtureEnvironment);
} // namespace
