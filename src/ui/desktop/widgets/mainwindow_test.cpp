#include "src/algorithms/protocol/testing/byte_matchers.h"
#include <cctype>
#include <ranges>
#include <string>
#include <vector>
#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QFileDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QInputDialog>
#include <QLabel>
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
#include "src/algorithms/memory/testing/memory_views.h"
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
#include "src/backend/definition/definition_catalog_session.h"
#include "src/backend/logging/logger_definition_service.h"
#include "src/backend/logging/logging_selection_use_case.h"
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
#include "src/backend/config/catalog.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"
#include "src/ui/desktop/config_fields.h"
#include "src/ui/desktop/hexedit/hexedit.h"
#include "src/ui/desktop/menu/testing/menu_snapshot.h"

using ::testing::_;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Optional;

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
    explicit ModalDriver(QString chooserChoice) : chooser_choice_(std::move(chooserChoice))
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
        if (answerMessageBox() || answerDialog())
        {
            return;
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

    // Answers the first open message box, noting any flash dialog before it.
    bool answerMessageBox()
    {
        const auto widgets = QApplication::topLevelWidgets();
        const auto box = std::ranges::find_if(widgets, [](const QWidget *widget)
                                              { return qobject_cast<const QMessageBox *>(widget) != nullptr; });
        if (std::any_of(widgets.begin(), box,
                        [](const QWidget *widget) { return widget->inherits("fastecu::flash::FlashDialog"); }))
        {
            unexpected_flash_dialog_count_ = 1;
        }
        if (box == widgets.end())
        {
            return false;
        }
        answer(*qobject_cast<QMessageBox *>(*box));
        return true;
    }

    // Picks the chooser option, counts and cancels the prompts below, and
    // accepts any other box.
    void answer(QMessageBox& messageBox)
    {
        if (messageBox.text() == kTcuChooserText && choose(messageBox))
        {
            return;
        }
        const std::array<std::pair<const char *, int *>, 3> cancelled{{
            {kTcuIgnitionText, &ignition_count_},
            {kLegacyEcuIgnitionText, &legacy_ecu_ignition_count_},
            {kPortableEcuIgnitionText, &portable_ecu_ignition_count_},
        }};
        if (const auto match =
                std::ranges::find_if(cancelled, [&](const auto& entry) { return messageBox.text() == entry.first; });
            match != cancelled.end())
        {
            ++*match->second;
            messageBox.done(QMessageBox::Cancel);
            return;
        }
        if (messageBox.text().startsWith(kNoChecksumModuleText))
        {
            ++checksum_warning_count_;
            messageBox.done(QMessageBox::Cancel);
            return;
        }
        if (messageBox.text() == kNoFileSelectedText)
        {
            ++no_file_selected_count_;
        }
        accepted_texts_ << messageBox.text();
        messageBox.accept();
    }

    // Rejects the chooser without a choice, or clicks the chosen option;
    // false when no option matches.
    bool choose(QMessageBox& messageBox)
    {
        saw_chooser_ = true;
        if (chooser_choice_.isEmpty())
        {
            messageBox.reject();
            return true;
        }
        const auto buttons = messageBox.buttons();
        const auto chosen = std::ranges::find_if(buttons, [this](const QAbstractButton *button)
                                                 { return button->text() == chooser_choice_; });
        if (chosen == buttons.end())
        {
            return false;
        }
        (*chosen)->click();
        return true;
    }

    // Closes the BIU window, and takes the continue-without-definition path.
    bool answerDialog()
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (dialog == nullptr || !dialog->isVisible())
            {
                continue;
            }
            if (qobject_cast<QFileDialog *>(dialog) != nullptr)
            {
                dialog->reject(); // cancel the file picker
                return true;
            }
            if (dialog->objectName() == "BiuOperationsSubaruWindow")
            {
                dialog->reject(); // the BIU window has no work to do here
                return true;
            }
            const auto buttons = dialog->findChildren<QRadioButton *>();
            if (std::ranges::any_of(buttons, [](const QRadioButton *button)
                                    { return button->text() == kContinueWithoutDefinitionText; }))
            {
                ++missing_definition_prompt_count_;
                dialog->reject(); // the continue-without path
                return true;
            }
        }
        return false;
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
bool startEcuOperations(MainWindow& window, const QString& cmdType)
{
    return QMetaObject::invokeMethod(&window, "startEcuOperations", Qt::DirectConnection, Q_ARG(QString, cmdType));
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
constexpr ActionName kEcuDefinitionManager{"actionEcuDefinitionManager"};
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
    for (const auto& [message, timestamp, linefeed] : recorder.Snapshot())
    {
        lines.emplace_back(message.toStdString(), timestamp, linefeed);
    }
    return lines;
}

using fastecu::config::ChecksumSupport;
using fastecu::config::ProtocolIn;
using fastecu::config::ProtocolSpec;
using fastecu::config::VehicleSpec;
using fastecu::ui::qs;

constexpr auto kWindowProtocols = std::to_array<ProtocolSpec>({
    {.name = "sub_tcu_denso_sh7058_can",
     .ecu = "Denso TCU SH7058",
     .mcu = "SH7058",
     .mode = "OBD2",
     .checksum = ChecksumSupport::kMissing,
     .read = true,
     .test_write = false,
     .write = true,
     .flash_transport = "iso15765,CAN",
     .log_transport = "K-Line",
     .log_protocol = "SSM",
     .kernel = "tcu_kernel.bin",
     .kernel_load_address = 0x100000U,
     .description = "Denso TCU SH7058"},
    {.name = "sub_ecu_denso_sh7058_can",
     .ecu = "Denso SH7058",
     .mcu = "SH7058",
     .mode = "OBD2",
     .checksum = ChecksumSupport::kCorrected,
     .read = true,
     .test_write = true,
     .write = true,
     .flash_transport = "iso15765,CAN",
     .log_transport = "K-Line",
     .log_protocol = "SSM",
     .kernel = "test-kernel.bin",
     .kernel_load_address = 0xFFFF3000U,
     .description = "Denso SH7058 CAN"},
    {.name = "sub_ecu_denso_sh7058_densocan",
     .ecu = "Denso SH7058",
     .mcu = "SH7058",
     .mode = "OBD2",
     .checksum = ChecksumSupport::kCorrected,
     .read = true,
     .test_write = true,
     .write = true,
     .flash_transport = "CAN",
     .log_transport = "K-Line",
     .log_protocol = "SSM",
     .kernel = "test-kernel.bin",
     .kernel_load_address = 0xFFFF3000U,
     .description = "Denso SH7058 DensoCAN"},
    {.name = "sub_ecu_denso_sh7058",
     .ecu = "Denso SH7058",
     .mcu = "SH7058",
     .mode = "OBD2",
     .checksum = ChecksumSupport::kCorrected,
     .read = true,
     .test_write = false,
     .write = true,
     .flash_transport = "K-Line",
     .log_transport = "K-Line",
     .log_protocol = "SSM",
     .kernel = "test-kernel.bin",
     .kernel_load_address = 0xFFFF3000U,
     .description = "Denso SH7058 K-Line"},
    {.name = "sub_ecu_denso_sh7058_can_future",
     .ecu = "Denso SH7058",
     .mcu = "SH7058",
     .mode = "OBD2",
     .checksum = ChecksumSupport::kCorrected,
     .read = true,
     .test_write = true,
     .write = true,
     .flash_transport = "iso15765,CAN",
     .log_transport = "K-Line",
     .log_protocol = "SSM",
     .kernel = "test-kernel.bin",
     .kernel_load_address = 0xFFFF3000U,
     .description = "Denso SH7058 CAN"},
    {.name = "sub_ecu_denso_sh7058_densocan_extra",
     .ecu = "Denso SH7058",
     .mcu = "SH7058",
     .mode = "OBD2",
     .checksum = ChecksumSupport::kCorrected,
     .read = true,
     .test_write = true,
     .write = true,
     .flash_transport = "iso15765,CAN",
     .log_transport = "K-Line",
     .log_protocol = "SSM",
     .kernel = "test-kernel.bin",
     .kernel_load_address = 0xFFFF3000U,
     .description = "Denso SH7058 CAN"},
    {.name = "sub_ecu_not_a_real_protocol",
     .ecu = "Denso SH7058",
     .mcu = "SH7058",
     .mode = "OBD2",
     .checksum = ChecksumSupport::kCorrected,
     .read = true,
     .test_write = true,
     .write = true,
     .flash_transport = "iso15765,CAN",
     .log_transport = "K-Line",
     .log_protocol = "SSM",
     .kernel = "test-kernel.bin",
     .kernel_load_address = 0xFFFF3000U,
     .description = "Denso SH7058 CAN"},
    {.name = "sub_ecu_denso_sh7058_can_checksum_na",
     .ecu = "Denso SH7058",
     .mcu = "SH7058",
     .mode = "OBD2",
     .checksum = ChecksumSupport::kMissing,
     .read = true,
     .test_write = true,
     .write = true,
     .flash_transport = "iso15765,CAN",
     .log_transport = "K-Line",
     .log_protocol = "SSM",
     .kernel = "test-kernel.bin",
     .kernel_load_address = 0xFFFF3000U,
     .description = "Denso SH7058 CAN"},
    {.name = "sub_ecu_test_cdbg",
     .ecu = "Denso SH7058",
     .mcu = "SH7058",
     .mode = "OBD2",
     .read = true,
     .flash_transport = "K-Line",
     .log_transport = "K-Line",
     .log_protocol = "CDBG",
     .description = "Test CDBG"},
    {.name = "sub_ecu_test_two_log_transports",
     .ecu = "Denso SH7058",
     .mcu = "SH7058",
     .mode = "OBD2",
     .read = true,
     .flash_transport = "K-Line",
     .log_transport = "K-Line,CAN",
     .log_protocol = "SSM",
     .description = "Test two log transports"},
});

constexpr auto kWindowVehicles = std::to_array<VehicleSpec>({
    {.id = "subaru-test-test--sub-tcu-denso-sh7058-can",
     .make = "Subaru",
     .model = "Test",
     .version = "Test",
     .type = "TCU",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_tcu_denso_sh7058_can")},
    {.id = "subaru-can-test--sub-ecu-denso-sh7058-can",
     .make = "Subaru",
     .model = "Can",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_denso_sh7058_can")},
    {.id = "subaru-densocan-test--sub-ecu-denso-sh7058-densocan",
     .make = "Subaru",
     .model = "DensoCan",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_denso_sh7058_densocan")},
    {.id = "subaru-kline-test--sub-ecu-denso-sh7058",
     .make = "Subaru",
     .model = "KLine",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_denso_sh7058")},
    {.id = "subaru-future-test--sub-ecu-denso-sh7058-can-future",
     .make = "Subaru",
     .model = "Future",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_denso_sh7058_can_future")},
    {.id = "subaru-extra-test--sub-ecu-denso-sh7058-densocan-extra",
     .make = "Subaru",
     .model = "Extra",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_denso_sh7058_densocan_extra")},
    {.id = "subaru-unsupported-test--sub-ecu-not-a-real-protocol",
     .make = "Subaru",
     .model = "Unsupported",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_not_a_real_protocol")},
    {.id = "subaru-checksumna-test--sub-ecu-denso-sh7058-can-checksum-na",
     .make = "Subaru",
     .model = "ChecksumNa",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_denso_sh7058_can_checksum_na")},
    {.id = "mitsubishi-colt-test--sub-ecu-denso-sh7058",
     .make = "Mitsubishi",
     .model = "Colt",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_denso_sh7058")},
    {.id = "nissan-test-test--sub-ecu-denso-sh7058",
     .make = "Nissan",
     .model = "Test",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_denso_sh7058")},
    {.id = "subaru-cdbg-test--sub-ecu-test-cdbg",
     .make = "Subaru",
     .model = "Cdbg",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_test_cdbg")},
    {.id = "subaru-twotransports-test--sub-ecu-test-two-log-transports",
     .make = "Subaru",
     .model = "TwoTransports",
     .version = "Test",
     .type = "",
     .kw = "",
     .hp = "",
     .fuel = "",
     .year = "",
     .protocol = ProtocolIn(kWindowProtocols, "sub_ecu_test_two_log_transports")},
});

constexpr fastecu::config::Catalog kWindowCatalog{kWindowProtocols, kWindowVehicles};

// The services DesktopComposition builds in the real app. The channels are
// left unwired: tests spy on them.
struct TestServices
{
    explicit TestServices(const QString& configRoot)
        : config_status(config.Initialize(configRoot.toStdString(), kTestApplication.version)),
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
            .logging_selection = logging_selection,
            .config_repository = file_repository,
            .file_action_events = events,
            .log = log_channel,
            .connection = adapter.Connection(),
            .logging_engine = logging_engine,
            .make_clock = make_clock,
        };
    }

    QtFileSystem file_system;
    QtResourceBundle resource_bundle;
    QtFileRepository file_repository;
    QtAtomicFileWriter file_writer;
    QtEventSink events;
    QtEventSink config_events;
    fastecu::config::ConfigSession config{kWindowCatalog, file_system, resource_bundle, file_repository, config_events};
    fastecu::Status config_status; // declared after `config`: initialized from it
    fastecu::definition::DefinitionService definition_service{file_system, file_repository, file_writer};
    fastecu::definition::DefinitionCatalogSession definition_catalogs;
    fastecu::calibration::RomOpenUseCase rom_open{
        definition_catalogs, definition_service, file_repository, file_system, events, config};
    fastecu::calibration::CalibrationWorkspace calibrations{rom_open};
    fastecu::calibration::RomSaveUseCase rom_save{file_repository, events};
    fastecu::logging::LoggerModel logger_model;
    fastecu::logging::LoggerDefinitionService logger_definitions{file_system, file_repository, file_writer};
    fastecu::logging::LoggingSelectionUseCase logging_selection{logger_definitions, logger_model};
    fastecu::ui::LogChannel log_channel;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.Fake(); // null if the fake backend failed to start
    fastecu::desktop::logging::LoggingEngine logging_engine;
    std::function<std::unique_ptr<fastecu::IClock>()> make_clock{[]() -> std::unique_ptr<fastecu::IClock>
                                                                 { return std::make_unique<QtClock>(); }};
};

} // namespace

class MainWindowTest : public ::testing::Test
{

  public:
    static void SetUpTestSuite();

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
            SCOPED_TRACE(::testing::PrintToString(name));
            ASSERT_TRUE(QFile::copy(source.filePath(name), root + suffix + name));
        }
    }

    // A window built the way DesktopComposition builds it, with the modal
    // driver dismissing any dialog the constructor raises.
    struct WindowUnderTest
    {
        std::optional<TestServices> services;
        std::optional<MainWindow> window;
    };

    void createWindow(WindowUnderTest& under)
    {
        ModalDriver constructorDriver{QString()};
        constructorDriver.start();
        under.services.emplace(config_root_->path());
        ASSERT_THAT(under.services->config_status, fastecu::testing::IsOk());
        ASSERT_TRUE(under.services->fake != nullptr);
        under.window.emplace(under.services->services());
        constructorDriver.stop();
    }

    static void selectOpenPort(MainWindow& window)
    {
        window.serial_ports_ = {"OpenPort 2.0"};
        window.serial_port_list_->clear();
        window.serial_port_list_->addItem("OpenPort 2.0");
        window.serial_port_list_->setCurrentIndex(0);
    }

    // Selects the fixture's last vehicle row using `protocol`.
    static void selectProtocol(MainWindow& window, const QString& protocol)
    {
        ASSERT_TRUE(window.config_session_->SelectByProtocolName(protocol.toStdString()));
    }

    // Selects the fixture's first Subaru row using `protocol`. The flash tests
    // dispatch only for Subaru/Mitsubishi, and sub_ecu_denso_sh7058's last row
    // is a Nissan one.
    static void selectSubaruProtocol(MainWindow& window, const QString& protocol)
    {
        const auto vehicles = window.config_session_->Vehicles();
        const auto it = std::ranges::find_if(
            vehicles, [&](const VehicleSpec& vehicle)
            { return vehicle.make == "Subaru" && vehicle.protocol->name == protocol.toStdString(); });
        ASSERT_TRUE(it != vehicles.end());
        ASSERT_THAT(window.config_session_->SelectRow(static_cast<std::size_t>(it - vehicles.begin())),
                    fastecu::testing::IsOk());
    }

    // Selects the fixture's first vehicle row of `make`.
    static void selectMake(MainWindow& window, const QString& make)
    {
        const auto vehicles = window.config_session_->Vehicles();
        const auto it = std::ranges::find(vehicles, make.toStdString(), &VehicleSpec::make);
        ASSERT_TRUE(it != vehicles.end());
        ASSERT_THAT(window.config_session_->SelectRow(static_cast<std::size_t>(it - vehicles.begin())),
                    fastecu::testing::IsOk());
    }

    // Points the window at one open port on the given make and log transport.
    static void prepareConnect(MainWindow& window, FakeBackend& fake, const QString& make, const QString& transport)
    {
        window.vbatt_timer_->stop();
        window.serial_ports_ = {"ttyUSB0"};
        window.serial_port_list_->clear();
        window.serial_port_list_->addItem("ttyUSB0");
        ASSERT_NO_FATAL_FAILURE(selectMake(window, make));
        window.config_session_->Settings().selected_log_transport = transport.toStdString();
        window.config_session_->Settings().selected_log_protocol = "SSM";
        ON_CALL(fake, OpenSerialPort()).WillByDefault(::testing::Return(QString("ttyUSB0")));
    }

    // The logging setup loggingCapturesTargetForEachRun and
    // loggingUsesTheSessionLogProtocol share: an identified ECU, a
    // "Logging" menu action, and one enabled `log_protocol` value.
    static void installLoggingFixture(MainWindow& window, fastecu::logging::LoggerDefinition definition,
                                      fastecu::logging::LoggerSelection selection)
    {
        *window.logger_model_ = fastecu::logging::LoggerModel{};
        window.logger_model_->InstallDefinition(std::move(definition));
        window.logger_model_->SetSelection(std::move(selection));
    }

    // Runs Save As on the window's selection and drives its file picker once:
    // `on_picker` runs while the picker is open, then the picker is cancelled
    // or accepts `target`. With `fail`, `target` becomes a directory as the
    // picker accepts, so the write fails. Informational notices are closed.
    // False when the picker never opened or timed out.
    static bool driveSaveAs(MainWindow& window, const QString& target, bool cancel, bool fail,
                            const std::function<void()>& onPicker = {})
    {
        QTimer timer;
        timer.setInterval(5);
        bool handled = false;
        bool timedOut = false;
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
                        timedOut = true;
                        dialog->reject();
                        return;
                    }
                    if (handled)
                    {
                        continue;
                    }
                    handled = true;
                    if (onPicker)
                    {
                        onPicker();
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
        window.saveCalibrationFileAs();
        timer.stop();
        if (timedOut)
        {
            qWarning() << "Save As dialog timed out for" << target;
        }
        return handled && !timedOut;
    }

    // Selects only the files-tree row at `index`, as a click would.
    static void selectFilesRow(MainWindow& window, int index)
    {
        QTreeWidget *files = window.ui_->calibrationFilesTreeWidget;
        for (int i = 0; i < files->topLevelItemCount(); ++i)
        {
            files->topLevelItem(i)->setSelected(i == index);
        }
    }

    static QAction *prepareLogging(MainWindow& window, const QString& logProtocol)
    {
        window.vbatt_timer_->stop();
        window.ecu_init_complete_ = true;
        window.protocol_ = logProtocol;
        QAction *action = menuAction(window, kToggleRealtime);
        installLoggingFixture(window,
                              {.parameters = {{.protocol = logProtocol.toStdString(),
                                               .id = "rpm",
                                               .name = "rpm",
                                               .address = "000010",
                                               .length = "1",
                                               .ecu_byte_index = "0",
                                               .ecu_bit = "0",
                                               .target = "ECU",
                                               .enabled = true,
                                               .conversions = {{"rpm", "x", "0", "0", "100", "1"}}}}},
                              {.protocol = logProtocol.toStdString(), .lower_panel_ids = {"rpm"}});
        return action;
    }

  public:
    static inline std::unique_ptr<QTemporaryDir> config_root_;

  protected:
    void checkExplicitConfigRootLoadsFixtureAndProvisionsDirectories();
    void checkWindowLogLinesReachTheLogChannel();
    void checkWindowEnablesFileLoggingThroughTheChannel();
    void checkHandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling(QString choice,
                                                                                  int expectedIgnitionCount);
    void checkFutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo(QString protocol);
    void checkRepresentativePortableRoutesReachFactoryBeforeLegacyFallback(QString protocol);
    void checkWriteWithoutASelectedCalibrationStopsVoltagePolling(QString command);
    void checkOtherMakesSkipDispatchButStillRunCleanup();
    void checkReadOfAnUnsupportedProtocolAddsNoCalibration();
    void checkCancellingTheChecksumWarningStopsVoltagePolling(QString command);
    void checkDefinitionlessOpenPromptsOnceAndAppliesPlaceholders();
    void checkClosingAMiddleRomKeepsLaterRomsAddressable();
    void checkWindowsOfAClosedRomAreInert();
    void checkHexEditorOutlivesItsRom();
    void checkClosingARomClosesAllOfItsWindows();
    void checkViewStateIsKeptPerRom();
    void checkWritePreparationRefreshesMetadataAndStatusLabel();
    void checkChecksumAndSaveUseATemporaryImage();
    void checkCalibrationLogsReachTheLogChannel();
    void checkSaveAsChangesSourceAndTreeOnlyAfterSuccess();
    void checkSaveAsUpdatesOriginalSessionAfterSelectionChanges();
    void checkSelectableSignalEditsItsEmittingSession();
    void checkFailedMapDecodeKeepsAnErrorView();
    void checkOpeningAMissingCalibrationFileReportsFailure();
    void checkCancellingTheCalibrationPickerReportsFailure();
    void checkStartingAnEcuOperationWithoutAPortWarns();
    // A second ROM opened beside the edited one: none, its session only, or
    // also its window of the same map.
    enum class AssignmentOther
    {
        kNone,
        kSession,
        kWindow,
    };
    enum class AssignmentAction
    {
        kSet,
        kPaste,
        kFineUp,
    };
    // One set-value, paste or increment edit of a 1D int16 map holding 10:
    // its scaling, what happens while the value dialog is open, and what the
    // ROM, the cell and any notice show afterwards.
    struct AssignmentCase
    {
        std::string from_byte = "x";
        std::string to_byte = "x";
        std::string fine_increment;
        std::string maximum;
        AssignmentOther other = AssignmentOther::kNone;
        bool write_during_dialog = false; // the map's bytes become 20
        bool select_other_during_dialog = false;
        bool close_during_dialog = false;
        QString typed = "x+1";
        AssignmentAction action = AssignmentAction::kSet;
        QString clipboard;
        std::optional<std::uint16_t> value; // absent: the edited session is closed
        QString cell;
        std::optional<bool> dirty;
        std::optional<bool> notice;
        QString notice_contains;
        QString notice_excludes;
    };
    void checkTypedAssignment(const AssignmentCase& c);
    static fastecu::definition::RomDefinition assignmentDefinition(const AssignmentCase& c);
    void openAssignmentMap(MainWindow& window, QMdiSubWindow *& subwindow, QTableWidget *& table);
    void addOtherCalibration(MainWindow& window, TestServices& services,
                             const fastecu::definition::RomDefinition& definition, const AssignmentCase& c,
                             QMdiSubWindow *subwindow, std::optional<fastecu::calibration::SessionId>& other,
                             QMdiSubWindow *& otherWindow);
    void answerAssignmentDialog(const AssignmentCase& c, MainWindow& window, TestServices& services,
                                fastecu::calibration::SessionId id, QMdiSubWindow *otherWindow, bool& answered,
                                QString& noticeText);
    void verifyAssignment(const AssignmentCase& c, TestServices& services, fastecu::calibration::SessionId id,
                          std::optional<fastecu::calibration::SessionId> other, QTableWidget *table,
                          const QString& noticeText);
    void checkWindowPreservesInjectedLoggingFactory();
    void checkLoggingCapturesTargetForEachRun();
    void checkLoggingDefinitionErrorShowsDetailAndLeavesStopped();
    void checkLoggingDisplayErrorContinuesOtherSamples();
    void checkChooserDialogsApplyAcceptedChoicesAndIgnoreCancellation(bool protocol, bool accept);
    void checkDefinitionManagerRemovesSelectedRowsAndSavesSurvivingOrder();
    void checkEditMenuOpensTheEcuDefinitionManager();
    void checkNumericWindowGeometryRestoresAndPersistsAcrossWindowStates();
    void checkAcceptedVehicleChoiceSelectsTheRowAndSavesIt();
    void checkCancelledVehicleChoiceChangesNothing();
    void checkAcceptedProtocolChoiceSelectsTheLastMatchingRow();
    void checkRomFlashMethodSelectsTheLastMatchingRow();
    void checkUnmatchedRomFlashMethodChangesNothing();
    void checkRestoreLoggingUiStateUnchecksLogging();
    void checkSetRealtimeStateChecksAndUnchecksLogging();
    void checkIdentificationDisablesLoggingAndConnectButNotDisconnect();
    void checkLogToFileActionDrivesWriteDatalogToFile();
    void checkFlashActionsFollowTheSelectedProtocolsCapabilities();
    void checkLoggingUsesTheSessionLogProtocol();
    void checkLoggingDefinitionErrorShowsContextAndLeavesStopped();
    void checkLoggingUnknownMutSelectionNeedsNoRomMatch();
    void checkLoggingCapabilityChecksumIsNotSupportEvidence();
    void checkLoggingSwitchOnlyDefaultsAreSelectable();
    void checkLoggingSelectionEditsWaitForNextRun();
    void checkLoggingSwitchValuesAndLiteralUnits();
    void checkLoggingUnavailableSelectionCanBeReplaced(bool knownUnsupported);
    void checkSelectedSerialPortIsEmptyWithoutPorts();
    void checkDtcWindowWithoutAPortWarnsInsteadOfCrashing();
    void checkRepeatedSaveFailuresLogOnceUntilASuccess();
    void checkBiuWindowRemembersTheOpenedPort();
    void checkDisconnectReturnsTheAdapterToIdle();
    void checkConnectOnAnotherMakeDisconnectsWithoutIdentifying();
    void checkSubaruKlineConnectIdentifiesOffTheUiThread();
    void checkSubaruConnectThatNeverAnswersDisconnectsAndRestoresControls();
    void checkDisconnectDuringIdentificationCancelsAndDropsTheResult();
    void checkLoggingSelectionWiringAndSupportPreservation();
    void checkLogTransportListRestoresTheSavedLogProtocol(const QString& protocol, const QString& flashTransport,
                                                          const QString& logTransport, const QString& logProtocol,
                                                          int expectedIndex);
    void checkVehicleChangeKeepsTheSavedLogProtocol();
    void checkSsmLoggingStartsAfterStartupWhenFlashAndLogTransportsDiffer();
    void checkSavingASelectionThatCannotBePersistedNoticesTheOperator();
    void checkTargetChangeInvalidatesIdentificationAndSupport();
    void checkLoggingDefinitionFailureIsNonfatal();
    void checkUnresolvedDisplaySlotsRemainVisibleAndUpdateTheirOriginalLabels();
    void checkChooserDuplicateLabelIdentity(int tab, QString kind);
    void checkLoggingStartWaitsForIdentification(bool targetIsEcu);
    void checkBatterySamplingDoesNotUseTheFacadeDuringIdentification();
    void checkLogWindowLinesReachEveryOpenOperationsWindow();
    void checkBatteryReadingReachesEveryOperationsWindowWithALabel();
    void checkOnlyTheOpenWindowsReceiveLogLines();
    void checkDataTerminalWindowAddsNoDebugLines();
    void checkWindowDestructionStopsLoggingWithoutAModalNotice();
    void checkWindowDestructionJoinsIdentificationWithoutContinuingLogging();
    void checkReplacementDuringSampleNotificationDiscardsOldBatch();
    void checkQueuedStopDuringRenderingPreventsTheCsvRow();
    void checkConnectStopsAnActiveLoggingWorkerBeforeIdentification();
    void checkConnectionEntryPointsStopIdentification(QString entryPoint);
    void checkNestedConnectDuringCapabilityNoticeKeepsEachContinuation();
    void checkMenuMatchesTheGolden();
    void checkEveryIconNamedByTheMenuResolves();
    void checkNoTwoActionsShareAShortcutAndNoneLostItsBinding();
    void checkToolbarKeepsMenuActionsBeforeTheTransportWidgets();
    void checkAStaleOrMalformedMenuCfgIsIgnored();
    void checkEveryMenuActionIsConnectedToTheWindow();
    void checkTriggeringLogToFileReachesItsHandler();
    void checkTuneActionsEditTheSelectionThroughTheirOwnHandlers();
    void checkCopyFromALargerMapPastesIntoASmallerOneThroughItsScaling();
};

void MainWindowTest::SetUpTestSuite()
{
    ASSERT_THAT(fastecu::config::CatalogProblems(kWindowCatalog), IsEmpty());
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
    const QString configDir =
        config_root_->path() + "/" + QString::fromStdString(kTestApplication.version) + "/config/";
    qInfo() << "Fixture config:" << configDir << "Qt home:" << QDir::homePath();
    ASSERT_TRUE(QDir().mkpath(configDir));
    ASSERT_TRUE(writeTextFile(configDir + "fastecu.cfg",

                              R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <software_settings>
    <setting name="window_size">
      <value width="maximized"/>
      <value height="maximized"/>
    </setting>
    <setting name="toolbar_iconsize"><value data="32"/></setting>
    <setting name="serial_port"><value data="OpenPort 2.0"/></setting>
    <setting name="vehicle_id"><value data="subaru-test-test--sub-tcu-denso-sh7058-can"/></setting>
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
    ASSERT_TRUE(writeTextFile(configDir + "logger.cfg",
                              R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <logger/>
</config>
)"));
    const QString kernelDir =
        config_root_->path() + "/" + QString::fromStdString(kTestApplication.version) + "/kernels/";
    ASSERT_TRUE(QDir().mkpath(kernelDir));
    // The kernels kWindowProtocols names: a flash request carries the selected
    // protocol, so the portable-route reads load these.
    ASSERT_TRUE(writeTextFile(kernelDir + "test-kernel.bin", "ABCD"));
    ASSERT_TRUE(writeTextFile(kernelDir + "tcu_kernel.bin", "ABCD"));
}

void MainWindowTest::checkExplicitConfigRootLoadsFixtureAndProvisionsDirectories()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    MainWindow& window = *under.window;

    const QString versionDir = config_root_->path() + "/" + window.software_version_ + "/";
    const fastecu::config::ConfigPaths paths = window.config_session_->ProvisionedPaths();
    ASSERT_EQ(paths.base_config_directory, config_root_->path().toStdString());
    ASSERT_EQ(paths.config_file, (versionDir + "config/fastecu.cfg").toStdString());
    ASSERT_EQ(window.config_session_->Vehicles().front().model, std::string("Test"));
    ASSERT_EQ(paths.syslog_files_directory, (versionDir + "syslogs/").toStdString());
    ASSERT_TRUE(QDir(versionDir + "syslogs").exists());
    ASSERT_TRUE(QDir(versionDir + "definitions").exists());
    ASSERT_TRUE(QFile::exists(QString::fromStdString(paths.config_file)));
}

TEST_F(MainWindowTest, explicitConfigRootLoadsFixtureAndProvisionsDirectories)
{
    ASSERT_NO_FATAL_FAILURE(checkExplicitConfigRootLoadsFixtureAndProvisionsDirectories());
}

void MainWindowTest::checkWindowLogLinesReachTheLogChannel()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    fastecu::testing::SignalRecorder lines{&services.log_channel, &fastecu::ui::LogChannel::logI};

    emit window.logI("probe line", true, false);

    ASSERT_EQ(lines.Count(), 1U);
    ASSERT_EQ(std::get<0>(lines.Snapshot().at(0)), QString("probe line"));
    ASSERT_EQ(std::get<1>(lines.Snapshot().at(0)), true);
    ASSERT_EQ(std::get<2>(lines.Snapshot().at(0)), false);
}

TEST_F(MainWindowTest, windowLogLinesReachTheLogChannel)
{
    ASSERT_NO_FATAL_FAILURE(checkWindowLogLinesReachTheLogChannel());
}

void MainWindowTest::checkWindowEnablesFileLoggingThroughTheChannel()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    fastecu::testing::SignalRecorder enables{&services.log_channel, &fastecu::ui::LogChannel::enableLogWriteToFile};
    MainWindow window{services.services()};
    constructorDriver.stop();

    ASSERT_THAT(enables.Snapshot(), ::testing::Contains(::testing::FieldsAre(true)));
}

TEST_F(MainWindowTest, windowEnablesFileLoggingThroughTheChannel)
{
    ASSERT_NO_FATAL_FAILURE(checkWindowEnablesFileLoggingThroughTheChannel());
}

struct HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase
{
    std::string name;
    QString choice;
    int expected_ignition_count;
};
std::vector<HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase>
handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingRows()
{
    std::vector<HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase> rows;

    rows.push_back(
        HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase{"chooser-cancelled", QString(), 0});
    rows.push_back(HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase{"relearn-declined",
                                                                                           QString("Relearn"), 1});

    return rows;
}
class HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingParameters,
                         ::testing::ValuesIn(handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingRows()),
                         [](const ::testing::TestParamInfo<
                             HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase>& paramInfo)
                         {
                             auto name = paramInfo.param.name;
                             for (char& c : name)
                             {
                                 if (!std::isalnum(static_cast<unsigned char>(c)))
                                 {
                                     c = '_';
                                 }
                             }
                             return name;
                         });
void MainWindowTest::checkHandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling(QString choice,
                                                                                              int expectedIgnitionCount)
{

    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, OpenSerialPort()).Times(0);
    EXPECT_CALL(*fake, WriteSerialData(::testing::_)).Times(0);
    EXPECT_CALL(*fake, WriteSerialDataEchoCheck(::testing::_)).Times(0);
    EXPECT_CALL(*fake, ReadSerialData(::testing::_)).Times(0);
    // Characterization: the call counts observed on master (before step
    // 6d) for both rows, pinned so the dispatch refactor cannot change them.
    EXPECT_CALL(*fake, ResetConnection()).Times(3);
    EXPECT_CALL(*fake, ChangePortSpeed(QStringLiteral("4800"))).Times(2);

    // A short poll period keeps the "timer stays stopped" check below quick.
    window.vbatt_timer_->setInterval(10);
    selectOpenPort(window);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_tcu_denso_sh7058_can"));

    // The TCU log lines are relayed through MainWindow's own LOG_* signals.
    fastecu::testing::SignalRecorder infoLines{&window, &MainWindow::logI};
    ModalDriver operationDriver{choice};
    operationDriver.start();
    ASSERT_TRUE(startEcuOperations(window, "read"));

    ASSERT_TRUE(operationDriver.sawChooser());
    ASSERT_EQ(operationDriver.ignitionCount(), expectedIgnitionCount);
    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.unexpectedFlashDialogCount(), 0);

    fastecu::testing::ProcessEventsFor(std::chrono::milliseconds(window.vbatt_timer_->interval() + 100));
    ASSERT_TRUE(!window.vbatt_timer_->isActive());
    ASSERT_THAT(window.calibrations_, IsEmpty());
    ASSERT_THAT(services.calibrations.Ids(), IsEmpty());
    const QString expectedLine = choice.isEmpty() ? "No option selected" : "Attempting TCU relearn";
    ASSERT_THAT(logLines(infoLines),
                ::testing::Contains(::testing::FieldsAre(expectedLine.toStdString(), ::testing::_, ::testing::_)));
}

TEST_P(HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingParameters,
       handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling)
{
    ASSERT_NO_FATAL_FAILURE(checkHandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling(
        GetParam().choice, GetParam().expected_ignition_count));
}

struct FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase
{
    std::string name;
    QString protocol;
};
std::vector<FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase>
futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoRows()
{
    std::vector<FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase> rows;

    rows.push_back(FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase{
        "future-can", QString("sub_ecu_denso_sh7058_can_future")});
    rows.push_back(FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase{
        "extra-densocan", QString("sub_ecu_denso_sh7058_densocan_extra")});

    return rows;
}
class FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase>
{
};
INSTANTIATE_TEST_SUITE_P(
    Rows, FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoParameters,
    ::testing::ValuesIn(futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoRows()),
    [](const ::testing::TestParamInfo<FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase>& paramInfo)
    {
        auto name = paramInfo.param.name;
        for (char& c : name)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)))
            {
                c = '_';
            }
        }
        return name;
    });
void MainWindowTest::checkFutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo(QString protocol)
{

    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, OpenSerialPort()).Times(0);
    EXPECT_CALL(*fake, WriteSerialData(::testing::_)).Times(0);
    EXPECT_CALL(*fake, WriteSerialDataEchoCheck(::testing::_)).Times(0);
    EXPECT_CALL(*fake, ReadSerialData(::testing::_)).Times(0);
    selectOpenPort(window);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, protocol));

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_TRUE(startEcuOperations(window, "read"));
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.legacyEcuIgnitionCount(), 0);
    ASSERT_EQ(operationDriver.portableEcuIgnitionCount(), 0);
    ASSERT_EQ(operationDriver.unexpectedFlashDialogCount(), 0);
}

TEST_P(FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoParameters,
       futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo)
{
    ASSERT_NO_FATAL_FAILURE(checkFutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo(GetParam().protocol));
}

struct RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackCase
{
    std::string name;
    QString protocol;
};
std::vector<RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackCase>
representativePortableRoutesReachFactoryBeforeLegacyFallbackRows()
{
    std::vector<RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackCase> rows;

    rows.push_back(RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackCase{
        "petrol", QString("sub_ecu_denso_sh7058_can")});
    rows.push_back(RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackCase{
        "densocan", QString("sub_ecu_denso_sh7058_densocan")});
    // The Denso SH705x K-Line family (sub_ecu_denso_sh7055_04*
    // and sub_ecu_denso_sh7058*) moved off FlashEcuSubaruDensoSH705xKline
    // onto this same portable factory path; see
    // exactDensoKlineIdsStillDispatchToTheLegacyKlineDialog in prior
    // revisions of this file for the characterization test this replaces.
    rows.push_back(RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackCase{"denso_sh705x_kline",
                                                                                    QString("sub_ecu_denso_sh7058")});

    return rows;
}
class RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackCase>
{
};
INSTANTIATE_TEST_SUITE_P(
    Rows, RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackParameters,
    ::testing::ValuesIn(representativePortableRoutesReachFactoryBeforeLegacyFallbackRows()),
    [](const ::testing::TestParamInfo<RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackCase>& paramInfo)
    {
        auto name = paramInfo.param.name;
        for (char& c : name)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)))
            {
                c = '_';
            }
        }
        return name;
    });
void MainWindowTest::checkRepresentativePortableRoutesReachFactoryBeforeLegacyFallback(QString protocol)
{

    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, OpenSerialPort()).Times(0);
    EXPECT_CALL(*fake, WriteSerialData(::testing::_)).Times(0);
    EXPECT_CALL(*fake, WriteSerialDataEchoCheck(::testing::_)).Times(0);
    EXPECT_CALL(*fake, ReadSerialData(::testing::_)).Times(0);
    selectOpenPort(window);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, protocol));

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_TRUE(startEcuOperations(window, "read"));
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.legacyEcuIgnitionCount(), 0);
    ASSERT_EQ(operationDriver.portableEcuIgnitionCount(), 1);
}

TEST_P(RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackParameters,
       representativePortableRoutesReachFactoryBeforeLegacyFallback)
{
    ASSERT_NO_FATAL_FAILURE(checkRepresentativePortableRoutesReachFactoryBeforeLegacyFallback(GetParam().protocol));
}

// Write and Test Write share one preflight, so each of its early returns is
// pinned for both commands.
struct WriteCommandCase
{
    std::string name;
    QString command;
};
std::vector<WriteCommandCase> writeCommandRows()
{
    return {WriteCommandCase{"write", "write"}, WriteCommandCase{"test_write", "test_write"}};
}
std::string writeCommandName(const ::testing::TestParamInfo<WriteCommandCase>& info)
{
    return info.param.name;
}

class WriteWithoutASelectedCalibrationStopsVoltagePollingParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<WriteCommandCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, WriteWithoutASelectedCalibrationStopsVoltagePollingParameters,
                         ::testing::ValuesIn(writeCommandRows()), writeCommandName);
void MainWindowTest::checkWriteWithoutASelectedCalibrationStopsVoltagePolling(QString command)
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, OpenSerialPort()).Times(0);
    EXPECT_CALL(*fake, WriteSerialData(::testing::_)).Times(0);
    EXPECT_CALL(*fake, ChangePortSpeed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
    selectOpenPort(window);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can"));

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_TRUE(startEcuOperations(window, command));
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.noFileSelectedCount(), 1);
    ASSERT_TRUE(!window.vbatt_timer_->isActive());
}

TEST_P(WriteWithoutASelectedCalibrationStopsVoltagePollingParameters,
       writeWithoutASelectedCalibrationStopsVoltagePolling)
{
    ASSERT_NO_FATAL_FAILURE(checkWriteWithoutASelectedCalibrationStopsVoltagePolling(GetParam().command));
}

void MainWindowTest::checkOtherMakesSkipDispatchButStillRunCleanup()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, OpenSerialPort()).Times(0);
    EXPECT_CALL(*fake, ChangePortSpeed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
    selectOpenPort(window);
    ASSERT_NO_FATAL_FAILURE(selectMake(window, "Nissan"));

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_TRUE(startEcuOperations(window, "read"));
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.unexpectedFlashDialogCount(), 0);
    ASSERT_TRUE(!window.vbatt_timer_->isActive());
}

TEST_F(MainWindowTest, otherMakesSkipDispatchButStillRunCleanup)
{
    ASSERT_NO_FATAL_FAILURE(checkOtherMakesSkipDispatchButStillRunCleanup());
}

void MainWindowTest::checkReadOfAnUnsupportedProtocolAddsNoCalibration()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;

    selectOpenPort(window);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_not_a_real_protocol"));
    ASSERT_EQ(window.calibrations_.size(), std::size_t{0});

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_TRUE(startEcuOperations(window, "read"));
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(window.calibrations_.size(), std::size_t{0});
    ASSERT_THAT(services.calibrations.Ids(), IsEmpty());
    ASSERT_EQ(window.ui_->calibrationFilesTreeWidget->topLevelItemCount(), 0);
}

TEST_F(MainWindowTest, readOfAnUnsupportedProtocolAddsNoCalibration)
{
    ASSERT_NO_FATAL_FAILURE(checkReadOfAnUnsupportedProtocolAddsNoCalibration());
}

class CancellingTheChecksumWarningStopsVoltagePollingParameters : public MainWindowTest,
                                                                  public ::testing::WithParamInterface<WriteCommandCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, CancellingTheChecksumWarningStopsVoltagePollingParameters,
                         ::testing::ValuesIn(writeCommandRows()), writeCommandName);
void MainWindowTest::checkCancellingTheChecksumWarningStopsVoltagePolling(QString command)
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, OpenSerialPort()).Times(0);
    EXPECT_CALL(*fake, WriteSerialData(::testing::_)).Times(0);
    EXPECT_CALL(*fake, ChangePortSpeed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
    selectOpenPort(window);
    QTemporaryDir roms;
    const QString romPath = writeRom(roms, "test.bin", '\x5a', 16);
    ASSERT_TRUE(!romPath.isEmpty());
    ModalDriver openDriver{QString()};
    openDriver.start();
    ASSERT_TRUE(window.openCalibrationFile(romPath));
    openDriver.stop();
    ASSERT_EQ(openDriver.missingDefinitionPromptCount(), 1);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can_checksum_na"));

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_TRUE(startEcuOperations(window, command));
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.checksumWarningCount(), 1);
    ASSERT_THAT(services.calibrations.Find(window.calibrations_.front().id)->File(), ::testing::Each(0x5a));
    ASSERT_TRUE(!window.vbatt_timer_->isActive());
}

TEST_P(CancellingTheChecksumWarningStopsVoltagePollingParameters, cancellingTheChecksumWarningStopsVoltagePolling)
{
    ASSERT_NO_FATAL_FAILURE(checkCancellingTheChecksumWarningStopsVoltagePolling(GetParam().command));
}

void MainWindowTest::checkDefinitionlessOpenPromptsOnceAndAppliesPlaceholders()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTemporaryDir roms;
    const QString path = writeRom(roms, "a.bin", '\x11');

    ModalDriver driver{QString()};
    driver.start();
    ASSERT_TRUE(window.openCalibrationFile(path));
    driver.stop();

    ASSERT_TRUE(!driver.timedOut());
    ASSERT_EQ(driver.missingDefinitionPromptCount(), 1);
    ASSERT_EQ(window.calibrations_.size(), std::size_t{1});
    QTreeWidgetItem *romInfo = window.ui_->calibrationDataTreeWidget->topLevelItem(0);
    ASSERT_EQ(romInfo->text(0), QString("ROM Info"));
    ASSERT_EQ(romInfo->child(0)->text(0), QString("XML ID: UnknownID"));
    ASSERT_EQ(romInfo->child(4)->text(0), "Make: " + qs(services.config.SelectedVehicle()->make));
    ASSERT_EQ(window.calibrations_.front().view.missing_definition_make,
              std::optional<QString>(qs(services.config.SelectedVehicle()->make)));
    ASSERT_EQ(services.calibrations.Find(window.calibrations_.front().id)->Source().display_name, std::string{"a.bin"});
    ASSERT_EQ(services.calibrations.Ids().size(), std::size_t{1});
    ASSERT_EQ(window.ui_->calibrationFilesTreeWidget->topLevelItem(0)->text(2),
              fastecu::ui::sessionKeyText(window.calibrations_.front().id));
}

TEST_F(MainWindowTest, definitionlessOpenPromptsOnceAndAppliesPlaceholders)
{
    ASSERT_NO_FATAL_FAILURE(checkDefinitionlessOpenPromptsOnceAndAppliesPlaceholders());
}

void MainWindowTest::checkClosingAMiddleRomKeepsLaterRomsAddressable()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_TRUE(window.openCalibrationFile(writeRom(roms, "a.bin", '\x0a')));
    ASSERT_TRUE(window.openCalibrationFile(writeRom(roms, "b.bin", '\x0b')));
    ASSERT_TRUE(window.openCalibrationFile(writeRom(roms, "c.bin", '\x0c')));
    driver.stop();
    ASSERT_EQ(window.calibrations_.size(), std::size_t{3});
    const auto a = window.calibrations_.at(0).id;
    const auto c = window.calibrations_.at(2).id;
    QTreeWidget *files = window.ui_->calibrationFilesTreeWidget;
    const QString cKey = files->topLevelItem(2)->text(2);

    for (int i = 0; i < files->topLevelItemCount(); ++i)
    {
        files->topLevelItem(i)->setSelected(i == 1);
    }
    window.closeCalibration();

    ASSERT_EQ(window.calibrations_.size(), std::size_t{2});
    ASSERT_EQ(services.calibrations.Ids(), (std::vector{a, c}));
    ASSERT_EQ(files->topLevelItemCount(), 2);
    ASSERT_EQ(files->topLevelItem(1)->text(2), cKey); // not renumbered
    ASSERT_TRUE(services.calibrations.Find(c) != nullptr);
    ASSERT_EQ(services.calibrations.Find(c)->Source().display_name, std::string{"c.bin"});
    ASSERT_EQ(services.calibrations.Find(c)->File()[0], std::uint8_t{0x0c});
}

TEST_F(MainWindowTest, closingAMiddleRomKeepsLaterRomsAddressable)
{
    ASSERT_NO_FATAL_FAILURE(checkClosingAMiddleRomKeepsLaterRomsAddressable());
}

void MainWindowTest::checkWindowsOfAClosedRomAreInert()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_TRUE(window.openCalibrationFile(writeRom(roms, "a.bin", '\x0a')));
    ASSERT_TRUE(window.openCalibrationFile(writeRom(roms, "b.bin", '\x0b')));
    driver.stop();
    const auto b = window.calibrations_.at(1).id;
    window.closeCalibration(); // b is selected after its open
    ASSERT_TRUE(services.calibrations.Find(b) == nullptr);

    const QString stale = fastecu::ui::sessionKeyText(b) + ",0,Idle";
    auto *content = new QWidget;
    QMdiSubWindow *sub = window.ui_->mdiArea->addSubWindow(content);
    sub->setObjectName(stale);
    content->setObjectName(stale);
    window.ui_->mdiArea->setActiveSubWindow(sub);
    QObject destroyedWindow;
    destroyedWindow.setObjectName(stale);

    window.setMaptablewidgetItems();
    window.selectableComboboxItemChanged("anything");
    window.checkboxStateChanged(2);
    window.closeCalibrationMap(&destroyedWindow);

    ASSERT_EQ(window.calibrations_.size(), std::size_t{1});
    ASSERT_EQ(window.ui_->calibrationFilesTreeWidget->topLevelItemCount(), 1);
}

TEST_F(MainWindowTest, windowsOfAClosedRomAreInert)
{
    ASSERT_NO_FATAL_FAILURE(checkWindowsOfAClosedRomAreInert());
}

void MainWindowTest::checkHexEditorOutlivesItsRom()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_TRUE(window.openCalibrationFile(writeRom(roms, "a.bin", '\x0a')));
    driver.stop();

    window.showHexEditor();
    window.closeCalibration();

    ASSERT_EQ(window.findChildren<HexEdit *>().size(), qsizetype{1});
    ASSERT_THAT(window.calibrations_, IsEmpty());
}

TEST_F(MainWindowTest, hexEditorOutlivesItsRom)
{
    ASSERT_NO_FATAL_FAILURE(checkHexEditorOutlivesItsRom());
}

void MainWindowTest::checkClosingARomClosesAllOfItsWindows()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_TRUE(window.openCalibrationFile(writeRom(roms, "a.bin", '\x0a')));
    ASSERT_TRUE(window.openCalibrationFile(writeRom(roms, "b.bin", '\x0b')));
    driver.stop();
    const QString aKey = fastecu::ui::sessionKeyText(window.calibrations_.at(0).id);
    const QString bKey = fastecu::ui::sessionKeyText(window.calibrations_.at(1).id);
    for (const QString& name : {aKey + ",0,X", aKey + ",1,Y", bKey + ",0,Z"})
    {
        auto *content = new QWidget;
        QMdiSubWindow *sub = window.ui_->mdiArea->addSubWindow(content);
        sub->setObjectName(name);
    }
    // The data tree still shows b; select a's row directly, as keyboard
    // navigation would, without rebuilding the data tree.
    QTreeWidget *files = window.ui_->calibrationFilesTreeWidget;
    files->topLevelItem(0)->setSelected(true);
    files->topLevelItem(1)->setSelected(false);

    window.closeCalibration();

    QStringList remaining;
    for (QMdiSubWindow *sub : window.ui_->mdiArea->subWindowList())
    {
        remaining << sub->objectName();
    }
    ASSERT_EQ(remaining, QStringList{bKey + ",0,Z"});
}

TEST_F(MainWindowTest, closingARomClosesAllOfItsWindows)
{
    ASSERT_NO_FATAL_FAILURE(checkClosingARomClosesAllOfItsWindows());
}

void MainWindowTest::checkViewStateIsKeptPerRom()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_TRUE(window.openCalibrationFile(writeRom(roms, "a.bin", '\x0a')));
    ASSERT_TRUE(window.openCalibrationFile(writeRom(roms, "b.bin", '\x0b')));
    driver.stop();
    QTreeWidget *files = window.ui_->calibrationFilesTreeWidget;
    QTreeWidget *data = window.ui_->calibrationDataTreeWidget;
    const auto selectRom = [&](int row)
    {
        for (int i = 0; i < files->topLevelItemCount(); ++i)
        {
            files->topLevelItem(i)->setSelected(i == row);
        }
        window.calibrationFilesTreewidgetItemSelected(files->topLevelItem(row));
    };

    selectRom(0);
    window.calibrationDataTreewidgetItemExpanded(data->topLevelItem(0)); // ROM Info
    selectRom(1);
    ASSERT_TRUE(!data->topLevelItem(0)->isExpanded());
    selectRom(0);
    ASSERT_TRUE(data->topLevelItem(0)->isExpanded());
    ASSERT_TRUE(window.calibrations_.at(0).view.rom_info_expanded);
    ASSERT_TRUE(!window.calibrations_.at(1).view.rom_info_expanded);
}

TEST_F(MainWindowTest, viewStateIsKeptPerRom)
{
    ASSERT_NO_FATAL_FAILURE(checkViewStateIsKeptPerRom());
}

void MainWindowTest::checkWritePreparationRefreshesMetadataAndStatusLabel()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    // The Subaru K-Line row's protocol is also used by a later Nissan row,
    // which the empty-method fill reselects.
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    window.status_bar_ecu_label_->setText("stale");
    fastecu::calibration::CalibrationSession session(
        fastecu::calibration::SessionId{41},
        fastecu::calibration::SessionContents{
            .source = {.display_name = "d.bin", .path = "/d.bin"},
            .image = fastecu::memory::testing::IdentityImage(std::vector<std::uint8_t>(16, 0)),
            .definition =
                fastecu::calibration::ResolvedDefinition{
                    .id = "D", .definition = {.format = fastecu::definition::DefinitionFormat::kEcuFlash}},
        });
    ASSERT_EQ(fastecu::ui::romInfoValue(fastecu::ui::romInfoValues(session), fastecu::ui::RomInfoRow::kFlashMethod),
              QString(""));

    // The 16-byte image draws the checksum command's bad-size notice, which
    // the driver accepts.
    const std::optional<fastecu::ui::PreparedWrite> prepared =
        window.calibration_operations_->prepareWrite(&session, "/kernels/");
    driver.stop();

    ASSERT_TRUE(!driver.timedOut());
    ASSERT_THAT(prepared, Optional(_));
    EXPECT_EQ(window.status_bar_ecu_label_->text().toStdString(), std::string{"Denso SH7058 K-Line "});
    EXPECT_EQ(services.config.SelectedVehicle()->make, std::string{"Nissan"});
    EXPECT_EQ(session.Protocol().flash_method, std::string{"sub_ecu_denso_sh7058"});
    EXPECT_EQ(fastecu::ui::romInfoValue(fastecu::ui::romInfoValues(session), fastecu::ui::RomInfoRow::kFlashMethod)
                  .toStdString(),
              std::string{"sub_ecu_denso_sh7058"});
    EXPECT_EQ(session.Protocol().mcu_type, std::string{"SH7058"});
    EXPECT_EQ(prepared->kernel_path, std::string{"/kernels/test-kernel.bin"});
    EXPECT_EQ(prepared->display_filename, std::string{"d.bin"});
    EXPECT_THAT(prepared->image, ::testing::ElementsAreArray(session.File()));
}

TEST_F(MainWindowTest, writePreparationRefreshesMetadataAndStatusLabel)
{
    ASSERT_NO_FATAL_FAILURE(checkWritePreparationRefreshesMetadataAndStatusLabel());
}

void MainWindowTest::checkChecksumAndSaveUseATemporaryImage()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    QTemporaryDir files;
    ASSERT_TRUE(writeTextFile(
        files.path() + "/definition.xml",
        R"(<rom><romid><xmlid>SAVE</xmlid><flashmethod>sub_ecu_denso_sh7058</flashmethod></romid></rom>)"));
    services.config.Settings().primary_definition_base = "ecuflash";
    services.config.Settings().use_ecuflash_definitions = "enabled";
    services.config.Settings().ecuflash_definition_files_directory = files.path().toStdString();
    ASSERT_THAT(services.definition_catalogs.RefreshIndex(fastecu::definition::DefinitionFormat::kEcuFlash),
                fastecu::testing::IsOk());
    const QString path = files.path() + "/save.bin";
    const auto opened = services.calibrations.AdoptReadImage({
        .rom = bytes::Bytes(1024UZ * 1024, 0),
        .filename = path.toStdString(),
        .rom_id = "SAVE",
        .protocol_name = "sub_ecu_denso_sh7058",
    });
    ASSERT_THAT(opened, fastecu::testing::IsOk());
    ASSERT_TRUE(window.addCalibration(opened->id));
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    auto *session = services.calibrations.Find(opened->id);
    ASSERT_TRUE(session != nullptr);
    ASSERT_TRUE(session->Definition() != nullptr);
    ASSERT_THAT(session->WriteBytes(0, bytes::Bytes{1}), fastecu::testing::IsOk());
    const bytes::Bytes original(session->File().begin(), session->File().end());
    // The expected bytes come from the backend dispatcher, independently of
    // the coordinator's selection plumbing.
    const VehicleSpec& vehicle = *services.config.SelectedVehicle();
    const fastecu::checksum::ChecksumCorrectionOutcome correction = fastecu::checksum::ApplyChecksumCorrection(
        fastecu::memory::testing::ImageAt(fastecu::memory::FlashAddress{0}, original),
        {
            .make = std::string(vehicle.make),
            .checksum_flag = std::string(fastecu::config::ChecksumFlag(vehicle.protocol->checksum)),
            .flash_method = std::string(vehicle.protocol->name),
            .mcu_type = session->Protocol().mcu_type,
            .rom_id = session->Protocol().rom_id,
        });
    ASSERT_EQ(correction.status, fastecu::checksum::ChecksumCorrectionOutcome::Status::kFamilyRan);
    ASSERT_THAT(correction.family_result, Optional(_));
    ASSERT_THAT(correction.corrected_file, Optional(_));
    const bytes::Bytes corrected = *correction.corrected_file;
    ASSERT_TRUE(corrected != original);
    ASSERT_TRUE(session->Dirty());

    window.saveCalibrationFile();
    EXPECT_THAT(services.file_repository.Read(path.toStdString()), fastecu::testing::IsOkAnd(corrected));
    ASSERT_THAT(session->File(), test_bytes::BytesEq(original));
    ASSERT_TRUE(!session->Dirty());
    ASSERT_THAT(session->WriteBytes(0, bytes::Bytes{2}), fastecu::testing::IsOk());
    const bytes::Bytes edited(session->File().begin(), session->File().end());
    // A directory is a deterministic failed file write on every platform.
    session->MarkSaved(files.path().toStdString());
    ASSERT_THAT(session->WriteBytes(0, bytes::Bytes{2}), fastecu::testing::IsOk());
    const auto source = session->Source();
    window.saveCalibrationFile();
    ASSERT_TRUE(session->Source() == source);
    ASSERT_TRUE(session->Dirty());
    ASSERT_THAT(session->File(), test_bytes::BytesEq(edited));
    driver.stop();
    ASSERT_TRUE(!driver.timedOut());
}

TEST_F(MainWindowTest, checksumAndSaveUseATemporaryImage)
{
    ASSERT_NO_FATAL_FAILURE(checkChecksumAndSaveUseATemporaryImage());
}

void MainWindowTest::checkCalibrationLogsReachTheLogChannel()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    QTemporaryDir files;
    // A non-ASCII name shows the window decodes the coordinator's UTF-8.
    const QString path = writeRom(files, QString::fromUtf8("caf\xc3\xa9.bin"), '\x22');
    ASSERT_TRUE(!path.isEmpty());
    ASSERT_TRUE(window.openCalibrationFile(path));
    driver.stop();
    ASSERT_TRUE(!driver.timedOut());
    auto *session = services.calibrations.Find(window.calibrations_.front().id);
    ASSERT_TRUE(session != nullptr);
    auto protocol = session->Protocol();
    protocol.mcu_type.clear(); // Unknown MCU: an error line and no checksum dialog.
    session->SetProtocol(protocol);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    fastecu::testing::SignalRecorder debug{&services.log_channel, &fastecu::ui::LogChannel::logD};
    fastecu::testing::SignalRecorder errors{&services.log_channel, &fastecu::ui::LogChannel::logE};

    window.saveCalibrationFile();

    EXPECT_THAT(logLines(debug),
                ::testing::ElementsAre(LogLine{"Protocol: sub_ecu_denso_sh7058", true, true},
                                       LogLine{"Make: Subaru", true, true}, LogLine{"Checksum: yes", true, true},
                                       LogLine{"ecuCalDef->FileName: caf\xc3\xa9.bin", true, true},
                                       LogLine{"ecuCalDef->FullFileName: " + session->Source().path, true, true}));
    EXPECT_THAT(logLines(errors), ::testing::ElementsAre(LogLine{"Unknown MCU type: ", true, true}));
}

TEST_F(MainWindowTest, calibrationLogsReachTheLogChannel)
{
    ASSERT_NO_FATAL_FAILURE(checkCalibrationLogsReachTheLogChannel());
}

void MainWindowTest::checkSaveAsChangesSourceAndTreeOnlyAfterSuccess()
{
    const bool nativeDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs =
        qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDisabled); });
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    QTemporaryDir files;
    const QString originalPath = writeRom(files, "original.bin", '\x11');
    ASSERT_TRUE(window.openCalibrationFile(originalPath));
    auto *session = services.calibrations.Find(window.calibrations_.front().id);
    ASSERT_TRUE(session != nullptr);
    auto protocol = session->Protocol();
    protocol.mcu_type.clear(); // Unknown MCU preserves bytes without a checksum dialog.
    session->SetProtocol(protocol);
    ASSERT_THAT(session->WriteBytes(0, bytes::Bytes{9}), fastecu::testing::IsOk());
    const auto originalSource = session->Source();
    QTreeWidgetItem *row = window.ui_->calibrationFilesTreeWidget->topLevelItem(0);
    const QString originalLabel = row->text(0);
    driver.stop();

    ASSERT_TRUE(driveSaveAs(window, {}, true, false));
    ASSERT_TRUE(session->Source() == originalSource);
    ASSERT_TRUE(session->Dirty());
    ASSERT_EQ(row->text(0), originalLabel);
    const QString blocked = files.path() + "/blocked.bin";
    ASSERT_TRUE(driveSaveAs(window, blocked, false, true));
    ASSERT_TRUE(session->Source() == originalSource);
    ASSERT_TRUE(session->Dirty());
    ASSERT_EQ(row->text(0), originalLabel);
    ASSERT_TRUE(driveSaveAs(window, files.path() + "/renamed.", false, false));
    ASSERT_EQ(session->Source().path, (files.path() + "/renamed.bin").toStdString());
    ASSERT_EQ(session->Source().display_name, std::string{"renamed.bin"});
    ASSERT_TRUE(!session->Dirty());
    ASSERT_TRUE(row->text(0).contains("renamed.bin"));
    const auto saved = services.file_repository.Read(session->Source().path);
    ASSERT_THAT(saved, fastecu::testing::IsOk());
    ASSERT_EQ(saved->at(0), std::uint8_t{9});
    ASSERT_EQ(session->File()[0], std::uint8_t{9});
}

TEST_F(MainWindowTest, saveAsChangesSourceAndTreeOnlyAfterSuccess)
{
    ASSERT_NO_FATAL_FAILURE(checkSaveAsChangesSourceAndTreeOnlyAfterSuccess());
}

// The picker is modal but the files tree can still change selection under
// it; the session selected when Save As started receives the save and the
// row label, never the row selected later.
void MainWindowTest::checkSaveAsUpdatesOriginalSessionAfterSelectionChanges()
{
    const bool nativeDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs =
        qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDisabled); });
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    QTemporaryDir files;
    ASSERT_TRUE(window.openCalibrationFile(writeRom(files, "a.bin", '\x0a')));
    ASSERT_TRUE(window.openCalibrationFile(writeRom(files, "b.bin", '\x0b')));
    driver.stop();
    ASSERT_EQ(window.calibrations_.size(), std::size_t{2});
    auto *a = services.calibrations.Find(window.calibrations_.at(0).id);
    auto *b = services.calibrations.Find(window.calibrations_.at(1).id);
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(b != nullptr);
    for (auto *session : {a, b})
    {
        auto protocol = session->Protocol();
        protocol.mcu_type.clear(); // Unknown MCU preserves bytes without a checksum dialog.
        session->SetProtocol(protocol);
    }
    QTreeWidgetItem *aRow = window.filesTreeItem(a->Id());
    QTreeWidgetItem *bRow = window.filesTreeItem(b->Id());
    ASSERT_TRUE(aRow != nullptr);
    ASSERT_TRUE(bRow != nullptr);
    const auto bSource = b->Source();
    const std::string bLabel = bRow->text(0).toStdString();
    ASSERT_NO_FATAL_FAILURE(selectFilesRow(window, 0));
    ASSERT_EQ(window.selectedCalibration(), a);

    const QString target = files.path() + "/renamed.bin";
    ASSERT_TRUE(driveSaveAs(window, target, false, false, [&] { selectFilesRow(window, 1); }));

    EXPECT_EQ(a->Source().path, target.toStdString());
    EXPECT_EQ(a->Source().display_name, std::string{"renamed.bin"});
    EXPECT_EQ(aRow->text(0).toStdString(), std::string{"renamed.bin"});
    EXPECT_TRUE(b->Source() == bSource);
    EXPECT_EQ(bRow->text(0).toStdString(), bLabel);
    EXPECT_EQ(window.selectedCalibration(), b);
    EXPECT_THAT(services.file_repository.Read(target.toStdString()),
                fastecu::testing::IsOkAnd(::testing::Each(std::uint8_t{0x0a})));
}

TEST_F(MainWindowTest, saveAsUpdatesOriginalSessionAfterSelectionChanges)
{
    ASSERT_NO_FATAL_FAILURE(checkSaveAsUpdatesOriginalSessionAfterSelectionChanges());
}

void MainWindowTest::checkSelectableSignalEditsItsEmittingSession()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    window.show();
    QApplication::processEvents();
    QTemporaryDir files;
    ASSERT_TRUE(writeTextFile(files.path() + "/selector.xml", R"(
<rom><romid><xmlid>SELECT</xmlid></romid>
<table name="Mode" category="Controls" address="0" type="1D" sizex="1" sizey="1">
<scaling storagetype="bloblist" endian="big"><data name="off" value="00"/><data name="on" value="01"/></scaling>
</table></rom>)"));
    services.config.Settings().primary_definition_base = "ecuflash";
    services.config.Settings().use_ecuflash_definitions = "enabled";
    services.config.Settings().ecuflash_definition_files_directory = files.path().toStdString();
    ASSERT_THAT(services.definition_catalogs.RefreshIndex(fastecu::definition::DefinitionFormat::kEcuFlash),
                fastecu::testing::IsOk());
    const auto openMap = [&](const QString& name) -> CalibrationMaps *
    {
        const auto opened = services.calibrations.AdoptReadImage({
            .rom = bytes::Bytes(16, 0),
            .filename = name.toStdString(),
            .rom_id = "SELECT",
        });
        if (!opened.has_value() || !window.addCalibration(opened->id))
        {
            return nullptr;
        }
        auto *fileTree = window.ui_->calibrationFilesTreeWidget;
        for (int row = 0; row < fileTree->topLevelItemCount(); ++row)
        {
            fileTree->topLevelItem(row)->setSelected(row == fileTree->topLevelItemCount() - 1);
        }
        window.calibrationFilesTreewidgetItemSelected(fileTree->topLevelItem(fileTree->topLevelItemCount() - 1));
        QTreeWidget *tree = window.ui_->calibrationDataTreeWidget;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
        {
            auto *category = tree->topLevelItem(i);
            if (category->text(0) == "Controls" && category->childCount() != 0)
            {
                tree->setCurrentItem(category->child(0));
                window.calibrationDataTreewidgetItemSelected(category->child(0));
                const auto windows = window.ui_->mdiArea->subWindowList();
                if (windows.isEmpty())
                {
                    return nullptr;
                }
                window.ui_->mdiArea->setActiveSubWindow(windows.back());
                return qobject_cast<CalibrationMaps *>(windows.back()->widget());
            }
        }
        return nullptr;
    };
    CalibrationMaps *first = openMap(files.path() + "/first.bin");
    ASSERT_TRUE(first != nullptr);
    const auto firstId = services.calibrations.Ids().front();
    CalibrationMaps *second = openMap(files.path() + "/second.bin");
    ASSERT_TRUE(second != nullptr);
    ASSERT_TRUE(first != second);
    const auto secondId = services.calibrations.Ids().back();
    ASSERT_TRUE(window.ui_->mdiArea->activeSubWindow()->widget() == second);

    // Emit from the inactive first map while the second ROM/window is selected.
    first->selectableComboboxItemChanged("enabled");

    ASSERT_EQ(services.calibrations.Find(firstId)->File()[0], std::uint8_t{1});
    ASSERT_TRUE(services.calibrations.Find(firstId)->Dirty());
    ASSERT_EQ(services.calibrations.Find(secondId)->File()[0], std::uint8_t{0});
    ASSERT_TRUE(!services.calibrations.Find(secondId)->Dirty());

    // A name no selection carries changes nothing, yet still refreshes the map
    // so the combo snaps back to the ROM's bytes.
    auto *combo = first->findChild<QComboBox *>();
    ASSERT_TRUE(combo != nullptr);
    const QString romSelection = combo->currentText();
    {
        const QSignalBlocker blocker(combo);
        combo->setCurrentIndex(combo->currentIndex() == 0 ? 1 : 0);
    }
    ASSERT_NE(combo->currentText(), romSelection);
    first->selectableComboboxItemChanged("missing");
    ASSERT_TRUE(first->findChild<QComboBox *>() != nullptr);
    EXPECT_EQ(first->findChild<QComboBox *>()->currentText(), romSelection);
    EXPECT_EQ(services.calibrations.Find(firstId)->File()[0], std::uint8_t{1});
    driver.stop();
    ASSERT_TRUE(!driver.timedOut());
}

TEST_F(MainWindowTest, selectableSignalEditsItsEmittingSession)
{
    ASSERT_NO_FATAL_FAILURE(checkSelectableSignalEditsItsEmittingSession());
}

void MainWindowTest::checkFailedMapDecodeKeepsAnErrorView()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    QTemporaryDir files;
    ASSERT_TRUE(window.openCalibrationFile(writeRom(files, "bad.bin", '\x11')));
    driver.stop();
    const auto id = window.calibrations_.front().id;
    auto *session = services.calibrations.Find(id);
    ASSERT_TRUE(session != nullptr);
    fastecu::definition::RomDefinition definition{.format = fastecu::definition::DefinitionFormat::kEcuFlash};
    definition.scalings.push_back({.name = "Raw"});
    fastecu::definition::CalibrationMap map;
    map.name = "Broken";
    map.category = "Controls";
    map.type = "1D";
    map.address = fastecu::memory::DefinitionAddress{1000}; // Beyond the opened 16-byte image.
    map.x_size = 1;
    map.y_size = 1;
    map.storage_type = fastecu::definition::StorageType::kUint8;
    map.scaling_name = "Raw";
    definition.maps.push_back(map);
    *session = fastecu::calibration::CalibrationSession(
        id,
        {
            .source = session->Source(),
            .image = fastecu::memory::testing::IdentityImage(bytes::Bytes(16, 0)),
            .definition = fastecu::calibration::ResolvedDefinition{.id = "BAD", .definition = std::move(definition)},
        });
    window.calibrationFilesTreewidgetItemSelected(window.ui_->calibrationFilesTreeWidget->topLevelItem(0));
    QTreeWidget *tree = window.ui_->calibrationDataTreeWidget;
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
    fastecu::testing::SignalRecorder errors(&window, &MainWindow::logE);
    tree->setCurrentItem(item);
    window.calibrationDataTreewidgetItemSelected(item);
    ASSERT_EQ(window.ui_->mdiArea->subWindowList().size(), 1);
    auto *view = window.ui_->mdiArea->subWindowList().front()->widget();
    ASSERT_NE(view->findChild<QLabel *>("mapDecodeError"), nullptr);
    EXPECT_FALSE(view->findChild<QTableWidget *>()->isEnabled());
    ASSERT_EQ(window.calibrations_.front().view.open_maps.size(), 1U);
    ASSERT_EQ(item->checkState(0), Qt::Checked);
    ASSERT_THAT(errors.Snapshot(), Not(IsEmpty()));
}

TEST_F(MainWindowTest, failedMapDecodeKeepsAnErrorView)
{
    ASSERT_NO_FATAL_FAILURE(checkFailedMapDecodeKeepsAnErrorView());
}

void MainWindowTest::checkOpeningAMissingCalibrationFileReportsFailure()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    QTemporaryDir files;
    EXPECT_FALSE(window.openCalibrationFile(files.filePath("absent.bin")));
    driver.stop();
}

TEST_F(MainWindowTest, openingAMissingCalibrationFileReportsFailure)
{
    ASSERT_NO_FATAL_FAILURE(checkOpeningAMissingCalibrationFileReportsFailure());
}

void MainWindowTest::checkCancellingTheCalibrationPickerReportsFailure()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    EXPECT_FALSE(window.openCalibrationFile(QString()));
    driver.stop();
    EXPECT_FALSE(driver.timedOut());
    EXPECT_EQ(driver.noFileSelectedCount(), 0);
    EXPECT_TRUE(driver.acceptedTexts().contains("No file selected"));
}

TEST_F(MainWindowTest, cancellingTheCalibrationPickerReportsFailure)
{
    ASSERT_NO_FATAL_FAILURE(checkCancellingTheCalibrationPickerReportsFailure());
}

void MainWindowTest::checkStartingAnEcuOperationWithoutAPortWarns()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    EXPECT_TRUE(startEcuOperations(window, "read"));
    driver.stop();
    EXPECT_TRUE(driver.acceptedTexts().contains("No serial port selected!"));
}

TEST_F(MainWindowTest, startingAnEcuOperationWithoutAPortWarns)
{
    ASSERT_NO_FATAL_FAILURE(checkStartingAnEcuOperationWithoutAPortWarns());
}

void MainWindowTest::checkWindowPreservesInjectedLoggingFactory()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    ASSERT_TRUE(services.fake != nullptr);
    bool called = false;
    services.logging_engine.RegisterProtocol("SSM",
                                             [&called](const fastecu::logging::LoggingRunSnapshot& snapshot)
                                             {
                                                 called = snapshot.Target() == fastecu::logging::LoggingTarget::kTcu;
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->BlockPollUntilCancelled();
                                                 return protocol;
                                             });
    MainWindow window{services.services()};
    constructorDriver.stop();
    // This test checks ownership, not UI error dialogs. The next test
    // drives actual menu dispatch and checks the selected target.
    QObject::disconnect(&services.logging_engine, nullptr, &window, nullptr);
    fastecu::logging::LoggerModel model;
    model.InstallDefinition({.parameters = {{.protocol = "SSM",
                                             .id = "rpm",
                                             .address = "10",
                                             .length = "1",
                                             .enabled = true,
                                             .conversions = {{"rpm", "x", "0", "0", "100", "1"}}}}});
    model.SetSelection({.protocol = "SSM", .lower_panel_ids = {"rpm"}});
    const auto snapshot = fastecu::logging::PrepareLoggingRun(model, fastecu::logging::LoggingProtocolId::kSsm, "SSM",
                                                              {.poll_timeout = std::chrono::milliseconds{50},
                                                               .car_silence_miss_threshold = 20,
                                                               .reconnect_attempt_threshold = 100,
                                                               .reconnect_retry_period = 20},
                                                              fastecu::logging::LoggingTarget::kTcu);
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    ASSERT_THAT(services.logging_engine.Start({"SSM"}, *snapshot), fastecu::testing::IsOk());
    services.logging_engine.Stop();
    ASSERT_TRUE(called);
}

TEST_F(MainWindowTest, windowPreservesInjectedLoggingFactory)
{
    ASSERT_NO_FATAL_FAILURE(checkWindowPreservesInjectedLoggingFactory());
}

void MainWindowTest::checkLoggingCapturesTargetForEachRun()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;
    window.config_session_->Settings().selected_log_protocol = "SSM";
    QAction *action = prepareLogging(window, "SSM");
    std::vector<bool> targets;
    services.logging_engine.RegisterProtocol("SSM",
                                             [&targets](const fastecu::logging::LoggingRunSnapshot& snapshot)
                                             {
                                                 targets.push_back(snapshot.Target() ==
                                                                   fastecu::logging::LoggingTarget::kEcu);
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->BlockPollUntilCancelled();
                                                 return protocol;
                                             });
    for (bool target : {true, false})
    {
        SCOPED_TRACE(::testing::PrintToString(target));
        window.ecu_radio_button_->setAutoExclusive(false);
        window.ecu_radio_button_->setChecked(target);
        window.connection_presentation_.identified({.ecu_id = target ? "ECU" : "TCU"});
        // trigger() toggles a checkable action, as a click does: start
        // unchecked so the handler sees Logging switched on.
        action->setChecked(false);
        ASSERT_TRUE(triggerMenu(window, kToggleRealtime));
        ASSERT_TRUE(action->isChecked());
        ASSERT_THAT(window.logging_run_->view().active_snapshot, Optional(_));
        ASSERT_EQ(window.logging_run_->view().active_snapshot->Target() == fastecu::logging::LoggingTarget::kEcu,
                  target);
        services.logging_engine.Stop();
    }
    ASSERT_EQ(targets, (std::vector<bool>{true, false}));
}

TEST_F(MainWindowTest, loggingCapturesTargetForEachRun)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingCapturesTargetForEachRun());
}

void MainWindowTest::checkLoggingDefinitionErrorShowsDetailAndLeavesStopped()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.config_session_->Settings().selected_log_protocol = "SSM";
    QAction *action = prepareLogging(window, "SSM");
    ASSERT_NE(action, nullptr);
    installLoggingFixture(window,
                          {.parameters = {{.protocol = "SSM",
                                           .id = "rpm",
                                           .name = "rpm",
                                           .address = "<b>bad</b>",
                                           .length = "1",
                                           .enabled = true,
                                           .conversions = {{"rpm", "x", "0", "0", "100", "1"}}}}},
                          {.protocol = "SSM", .lower_panel_ids = {"rpm"}});
    int factoryCalls = 0;
    services.logging_engine.RegisterProtocol("SSM",
                                             [&factoryCalls](const fastecu::logging::LoggingRunSnapshot&)
                                             {
                                                 ++factoryCalls;
                                                 return std::make_unique<ScriptedLoggingProtocol>();
                                             });
    fastecu::testing::SignalRecorder errors{&window, &MainWindow::logE};
    QString dialogText;
    Qt::TextFormat dialogFormat = Qt::AutoText;
    QTimer dialogDriver;
    dialogDriver.setInterval(5);
    QObject::connect(&dialogDriver, &QTimer::timeout, &window,
                     [&]
                     {
                         if (auto *notice = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                             notice != nullptr)
                         {
                             dialogText = notice->text();
                             dialogFormat = notice->textFormat();
                             notice->accept();
                         }
                     });
    dialogDriver.start();
    action->setChecked(false);
    ASSERT_TRUE(triggerMenu(window, kToggleRealtime));
    dialogDriver.stop();

    EXPECT_THAT(dialogText.toStdString(), ::testing::HasSubstr("SSM parameter rpm"));
    EXPECT_THAT(dialogText.toStdString(), ::testing::HasSubstr("address"));
    EXPECT_THAT(dialogText.toStdString(), ::testing::HasSubstr("<b>bad</b>"));
    EXPECT_THAT(dialogText.toStdString(), ::testing::HasSubstr("hexadecimal"));
    EXPECT_EQ(dialogFormat, Qt::PlainText);
    const auto loggedErrors = errors.Snapshot();
    ASSERT_EQ(loggedErrors.size(), 1U);
    const auto dialogPrefix = QStringLiteral("Unable to start logging:\n");
    ASSERT_TRUE(dialogText.startsWith(dialogPrefix));
    EXPECT_EQ(std::get<0>(loggedErrors.front()),
              QStringLiteral("Logging session failed to start: ") + dialogText.mid(dialogPrefix.size()));
    EXPECT_TRUE(std::get<1>(loggedErrors.front()));
    EXPECT_TRUE(std::get<2>(loggedErrors.front()));
    EXPECT_EQ(factoryCalls, 0);
    EXPECT_FALSE(services.logging_engine.IsRunning());
    EXPECT_THAT(window.logging_run_->view().active_snapshot, Eq(std::nullopt));
    EXPECT_FALSE(action->isChecked());
    EXPECT_FALSE(window.logging_state_);
}

TEST_F(MainWindowTest, loggingDefinitionErrorShowsDetailAndLeavesStopped)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingDefinitionErrorShowsDetailAndLeavesStopped());
}

struct ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase
{
    std::string name;
    bool protocol;
    bool accept;
};
std::vector<ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase>
chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationRows()
{
    std::vector<ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase> rows;

    rows.push_back(ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase{"vehicle-accept", false, true});
    rows.push_back(ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase{"vehicle-cancel", false, false});
    rows.push_back(ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase{"protocol-accept", true, true});
    rows.push_back(ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase{"protocol-cancel", true, false});

    return rows;
}
class ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase>
{
};
INSTANTIATE_TEST_SUITE_P(
    Rows, ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationParameters,
    ::testing::ValuesIn(chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationRows()),
    [](const ::testing::TestParamInfo<ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase>& paramInfo)
    {
        auto name = paramInfo.param.name;
        for (char& c : name)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)))
            {
                c = '_';
            }
        }
        return name;
    });
namespace
{
bool selectTreeText(QTreeWidget *tree, const QString& text)
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
}

// Selects `target`'s protocol, or its make, model and version row (column 12
// holds the row number).
bool selectChoice(QDialog *dialog, bool protocol, const fastecu::config::VehicleSpec& target)
{
    if (protocol)
    {
        return selectTreeText(dialog->findChild<QTreeWidget *>("treeWidget"), qs(target.protocol->name));
    }
    if (!selectTreeText(dialog->findChild<QTreeWidget *>("car_make_tree_widget"), qs(target.make)) ||
        !selectTreeText(dialog->findChild<QTreeWidget *>("car_model_tree_widget"), qs(target.model)))
    {
        return false;
    }
    auto *versions = dialog->findChild<QTreeWidget *>("car_version_tree_widget");
    for (int i = 0; versions != nullptr && i < versions->topLevelItemCount(); ++i)
    {
        if (versions->topLevelItem(i)->text(12) == "1")
        {
            versions->setCurrentItem(versions->topLevelItem(i));
            return true;
        }
    }
    return false;
}

// The row a choice of `target` selects: itself for a vehicle, the last row
// with its protocol for a protocol.
std::size_t chosenRow(const fastecu::config::ConfigSession& config, bool protocol)
{
    const auto& target = config.Vehicles()[1];
    std::size_t expected = 1;
    for (std::size_t row = 0; protocol && row < config.Vehicles().size(); ++row)
    {
        if (config.Vehicles()[row].protocol == target.protocol)
        {
            expected = row;
        }
    }
    return expected;
}
} // namespace

void MainWindowTest::checkChooserDialogsApplyAcceptedChoicesAndIgnoreCancellation(bool protocol, bool accept)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_NO_FATAL_FAILURE(copyFixtureConfig(root.path()));
    TestServices services{root.path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    ASSERT_THAT(services.config.SelectRow(0), fastecu::testing::IsOk());
    ASSERT_THAT(services.config.Save(), fastecu::testing::IsOk());
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    MainWindow window{services.services()};
    constructorDriver.stop();
    const auto target = services.config.Vehicles()[1];
    const std::size_t expected = chosenRow(services.config, protocol);
    QTimer driver;
    QElapsedTimer deadline;
    bool driven = false;
    bool unexpected = false;
    bool timedOut = false;
    QObject::connect(&driver, &QTimer::timeout,
                     [&]
                     {
                         auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                         if (deadline.elapsed() > 3000)
                         {
                             timedOut = true;
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
                         const bool selected = dialog->inherits(protocol ? "ProtocolSelect" : "VehicleSelect") &&
                                               selectChoice(dialog, protocol, target);
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
        window.selectProtocol();
    }
    else
    {
        window.selectVehicle();
    }
    driver.stop();
    ASSERT_TRUE(driven);
    ASSERT_TRUE(!unexpected);
    ASSERT_TRUE(!timedOut);
    ASSERT_EQ(*services.config.SelectedRow(), accept ? expected : std::size_t{0});
    TestServices reread{root.path()};
    ASSERT_THAT(reread.config_status, fastecu::testing::IsOk());
    ASSERT_EQ(*reread.config.SelectedRow(), accept ? expected : std::size_t{0});
}

TEST_P(ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationParameters,
       chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation)
{
    ASSERT_NO_FATAL_FAILURE(
        checkChooserDialogsApplyAcceptedChoicesAndIgnoreCancellation(GetParam().protocol, GetParam().accept));
}

void MainWindowTest::checkDefinitionManagerRemovesSelectedRowsAndSavesSurvivingOrder()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_NO_FATAL_FAILURE(copyFixtureConfig(root.path()));
    TestServices services{root.path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    services.config.Settings().romraider_definition_files = {"/first.xml", "/middle.xml", "/last.xml"};
    ASSERT_THAT(services.config.Save(), fastecu::testing::IsOk());
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTimer driver;
    QElapsedTimer deadline;
    bool driven = false;
    bool unexpected = false;
    bool unchangedWithoutSelection = false;
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
                         unchangedWithoutSelection = services.config.Settings().romraider_definition_files ==
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
    window.ecuDefinitionManager();
    driver.stop();
    ASSERT_TRUE(driven);
    ASSERT_TRUE(!unexpected);
    ASSERT_TRUE(unchangedWithoutSelection);
    ASSERT_EQ(displayed, (QStringList{"/first.xml", "/last.xml"}));
    const std::vector<std::string> expected{"/first.xml", "/last.xml"};
    ASSERT_EQ(services.config.Settings().romraider_definition_files, expected);
    TestServices reread{root.path()};
    ASSERT_THAT(reread.config_status, fastecu::testing::IsOk());
    ASSERT_EQ(reread.config.Settings().romraider_definition_files, expected);
}

TEST_F(MainWindowTest, definitionManagerRemovesSelectedRowsAndSavesSurvivingOrder)
{
    ASSERT_NO_FATAL_FAILURE(checkDefinitionManagerRemovesSelectedRowsAndSavesSurvivingOrder());
}

void MainWindowTest::checkEditMenuOpensTheEcuDefinitionManager()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.vbatt_timer_->stop();
    // No calibration is open and no ECU is connected: the dialog needs only settings.
    QAction *action = menuAction(window, kEcuDefinitionManager);
    ASSERT_NE(action, nullptr);
    ASSERT_TRUE(window.ui_->menuEdit->actions().contains(action));

    QTimer driver;
    bool opened = false;
    QObject::connect(&driver, &QTimer::timeout,
                     [&]
                     {
                         auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                         if (dialog != nullptr)
                         {
                             opened = dialog->objectName() == "ecu_definition_manager_dialog";
                             dialog->reject();
                         }
                     });
    driver.start(5);
    ASSERT_TRUE(triggerMenu(window, kEcuDefinitionManager));
    driver.stop();
    EXPECT_TRUE(opened);
}

TEST_F(MainWindowTest, editMenuOpensTheEcuDefinitionManager)
{
    ASSERT_NO_FATAL_FAILURE(checkEditMenuOpensTheEcuDefinitionManager());
}

void MainWindowTest::checkNumericWindowGeometryRestoresAndPersistsAcrossWindowStates()
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_NO_FATAL_FAILURE(copyFixtureConfig(root.path()));
    TestServices services{root.path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    services.config.Settings().window_width = "900";
    services.config.Settings().window_height = "700";
    ASSERT_THAT(services.config.Save(), fastecu::testing::IsOk());
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_EQ(window.size(), QSize(900, 700));
    window.show();
    QCoreApplication::processEvents();
    // Exercise a layout minimum larger than the old request on every host,
    // as the Windows offscreen layout does with its platform font metrics.
    window.setMinimumSize(window.minimumSize().expandedTo(QSize(1081, 750)));
    const QSize resizedSize = QSize(950, 750).expandedTo(window.minimumSize());
    window.resize(resizedSize);
    QCoreApplication::processEvents();
    ASSERT_EQ(window.size(), resizedSize);
    TestServices resized{root.path()};
    ASSERT_THAT(resized.config_status, fastecu::testing::IsOk());
    ASSERT_EQ(resized.config.Settings().window_width, std::to_string(resizedSize.width()));
    ASSERT_EQ(resized.config.Settings().window_height, std::to_string(resizedSize.height()));
    window.showMaximized();
    QCoreApplication::processEvents();
    ASSERT_EQ(services.config.Settings().window_width, std::string("maximized"));
    ASSERT_EQ(services.config.Settings().window_height, std::string("maximized"));
    TestServices maximized{root.path()};
    ASSERT_THAT(maximized.config_status, fastecu::testing::IsOk());
    ASSERT_EQ(maximized.config.Settings().window_width, std::string("maximized"));
    window.showNormal();
    QCoreApplication::processEvents();
    ASSERT_EQ(services.config.Settings().window_width, std::to_string(window.width()));
    ASSERT_EQ(services.config.Settings().window_height, std::to_string(window.height()));
    TestServices restored{root.path()};
    ASSERT_THAT(restored.config_status, fastecu::testing::IsOk());
    ASSERT_EQ(restored.config.Settings().window_width, services.config.Settings().window_width);
    ASSERT_EQ(restored.config.Settings().window_height, services.config.Settings().window_height);
}

TEST_F(MainWindowTest, numericWindowGeometryRestoresAndPersistsAcrossWindowStates)
{
    ASSERT_NO_FATAL_FAILURE(checkNumericWindowGeometryRestoresAndPersistsAcrossWindowStates());
}

void MainWindowTest::checkAcceptedVehicleChoiceSelectsTheRowAndSavesIt()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    const std::string flashTransport = services.config.Settings().selected_flash_transport;
    const std::string logTransport = services.config.Settings().selected_log_transport;

    window.applyVehicleChoice(QDialog::Accepted, 1);

    ASSERT_EQ(*services.config.SelectedRow(), std::size_t{1});
    ASSERT_EQ(services.config.Settings().selected_log_protocol, std::string("SSM"));
    ASSERT_EQ(services.config.Settings().selected_flash_transport, flashTransport);
    ASSERT_EQ(services.config.Settings().selected_log_transport, logTransport);

    // Saved: a fresh session over the same root restores row 1.
    QtEventSink rereadEvents;
    fastecu::config::ConfigSession reread{kWindowCatalog, services.file_system, services.resource_bundle,
                                          services.file_repository, rereadEvents};
    ASSERT_THAT(reread.Initialize(config_root_->path().toStdString(), kTestApplication.version),
                fastecu::testing::IsOk());
    ASSERT_EQ(reread.Settings().selected_vehicle_id, std::string(kWindowVehicles[1].id));
}

TEST_F(MainWindowTest, acceptedVehicleChoiceSelectsTheRowAndSavesIt)
{
    ASSERT_NO_FATAL_FAILURE(checkAcceptedVehicleChoiceSelectsTheRowAndSavesIt());
}

void MainWindowTest::checkCancelledVehicleChoiceChangesNothing()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    const auto before = services.config.Settings();

    window.applyVehicleChoice(QDialog::Rejected, 1);

    ASSERT_TRUE(services.config.Settings() == before);
}

TEST_F(MainWindowTest, cancelledVehicleChoiceChangesNothing)
{
    ASSERT_NO_FATAL_FAILURE(checkCancelledVehicleChoiceChangesNothing());
}

void MainWindowTest::checkAcceptedProtocolChoiceSelectsTheLastMatchingRow()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    const auto vehicles = services.config.Vehicles();
    std::size_t last = 0;
    for (std::size_t i = 0; i < vehicles.size(); ++i)
    {
        if (vehicles[i].protocol->name == "sub_ecu_denso_sh7058")
        {
            last = i;
        }
    }

    window.applyProtocolChoice(QDialog::Accepted, std::string("sub_ecu_denso_sh7058"));

    ASSERT_EQ(*services.config.SelectedRow(), last);
}

TEST_F(MainWindowTest, acceptedProtocolChoiceSelectsTheLastMatchingRow)
{
    ASSERT_NO_FATAL_FAILURE(checkAcceptedProtocolChoiceSelectsTheLastMatchingRow());
}

void MainWindowTest::checkRomFlashMethodSelectsTheLastMatchingRow()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.updateProtocolInfo("sub_ecu_denso_sh7058");

    ASSERT_EQ(*services.config.SelectedRow(), std::size_t{9}); // rows 3, 8, 9 match; the last wins
    ASSERT_EQ(services.config.SelectedVehicle()->make, std::string("Nissan"));
}

TEST_F(MainWindowTest, romFlashMethodSelectsTheLastMatchingRow)
{
    ASSERT_NO_FATAL_FAILURE(checkRomFlashMethodSelectsTheLastMatchingRow());
}

void MainWindowTest::checkUnmatchedRomFlashMethodChangesNothing()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    const auto before = services.config.Settings();
    window.updateProtocolInfo("no_such_protocol");

    ASSERT_TRUE(services.config.Settings() == before);
}

TEST_F(MainWindowTest, unmatchedRomFlashMethodChangesNothing)
{
    ASSERT_NO_FATAL_FAILURE(checkUnmatchedRomFlashMethodChangesNothing());
}

void MainWindowTest::checkRestoreLoggingUiStateUnchecksLogging()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    QAction *logging = menuAction(window, kToggleRealtime);
    ASSERT_NE(logging, nullptr);
    ASSERT_TRUE(logging->isCheckable());
    logging->setChecked(true);
    window.logging_state_ = true;

    window.restoreLoggingUiState();

    EXPECT_FALSE(logging->isChecked());
    EXPECT_FALSE(window.logging_state_);
}

TEST_F(MainWindowTest, restoreLoggingUiStateUnchecksLogging)
{
    ASSERT_NO_FATAL_FAILURE(checkRestoreLoggingUiStateUnchecksLogging());
}

void MainWindowTest::checkSetRealtimeStateChecksAndUnchecksLogging()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    QAction *logging = menuAction(window, kToggleRealtime);
    ASSERT_NE(logging, nullptr);

    window.setRealtimeState(true);
    EXPECT_TRUE(logging->isChecked());
    window.setRealtimeState(false);
    EXPECT_FALSE(logging->isChecked());
}

TEST_F(MainWindowTest, setRealtimeStateChecksAndUnchecksLogging)
{
    ASSERT_NO_FATAL_FAILURE(checkSetRealtimeStateChecksAndUnchecksLogging());
}

void MainWindowTest::checkIdentificationDisablesLoggingAndConnectButNotDisconnect()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    QAction *logging = menuAction(window, kToggleRealtime);
    QAction *connectAction = menuAction(window, kConnectToEcu);
    QAction *disconnectAction = menuAction(window, kDisconnectFromEcu);
    ASSERT_NE(logging, nullptr);
    ASSERT_NE(connectAction, nullptr);
    ASSERT_NE(disconnectAction, nullptr);

    window.connection_presentation_.setControlsLocked(true);
    EXPECT_FALSE(logging->isEnabled());
    EXPECT_FALSE(connectAction->isEnabled());
    EXPECT_TRUE(disconnectAction->isEnabled());

    window.connection_presentation_.setControlsLocked(false);
    EXPECT_TRUE(logging->isEnabled());
    EXPECT_TRUE(connectAction->isEnabled());
    EXPECT_TRUE(disconnectAction->isEnabled());
}

TEST_F(MainWindowTest, identificationDisablesLoggingAndConnectButNotDisconnect)
{
    ASSERT_NO_FATAL_FAILURE(checkIdentificationDisablesLoggingAndConnectButNotDisconnect());
}

void MainWindowTest::checkLogToFileActionDrivesWriteDatalogToFile()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    QAction *logToFile = menuAction(window, kLogToFile);
    ASSERT_NE(logToFile, nullptr);
    ASSERT_TRUE(logToFile->isCheckable());

    logToFile->setChecked(true);
    window.toggleLogToFile();
    EXPECT_TRUE(window.logging_run_->view().csv_enabled);

    logToFile->setChecked(false);
    window.toggleLogToFile();
    EXPECT_FALSE(window.logging_run_->view().csv_enabled);
}

TEST_F(MainWindowTest, logToFileActionDrivesWriteDatalogToFile)
{
    ASSERT_NO_FATAL_FAILURE(checkLogToFileActionDrivesWriteDatalogToFile());
}

void MainWindowTest::checkFlashActionsFollowTheSelectedProtocolsCapabilities()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    QAction *readAction = menuAction(window, kReadRomFromEcu);
    QAction *testWriteAction = menuAction(window, kTestWriteRomToEcu);
    QAction *writeAction = menuAction(window, kWriteRomToEcu);
    ASSERT_NE(readAction, nullptr);
    ASSERT_NE(testWriteAction, nullptr);
    ASSERT_NE(writeAction, nullptr);

    // A protocol with every capability enables all three...
    ASSERT_NO_FATAL_FAILURE(selectProtocol(window, "sub_ecu_denso_sh7058_can"));
    window.setFlashArrowState();
    ASSERT_TRUE(readAction->isEnabled());
    ASSERT_TRUE(testWriteAction->isEnabled());
    ASSERT_TRUE(writeAction->isEnabled());

    // ...and one without test write disables only that action.
    ASSERT_NO_FATAL_FAILURE(selectProtocol(window, "sub_ecu_denso_sh7058"));
    window.setFlashArrowState();
    ASSERT_TRUE(readAction->isEnabled());
    ASSERT_TRUE(!testWriteAction->isEnabled());
    ASSERT_TRUE(writeAction->isEnabled());
}

TEST_F(MainWindowTest, flashActionsFollowTheSelectedProtocolsCapabilities)
{
    ASSERT_NO_FATAL_FAILURE(checkFlashActionsFollowTheSelectedProtocolsCapabilities());
}

void MainWindowTest::checkLoggingDefinitionErrorShowsContextAndLeavesStopped()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    constructorDriver.stop();
    auto *action = prepareLogging(window, "SSM");
    action->setChecked(true);
    window.logging_state_ = true;
    window.config_session_->Settings().selected_log_protocol = "SSM";
    auto definition = window.logger_model_->Definition();
    definition.parameters.front().conversions.front().format = "<b>bad</b>";
    installLoggingFixture(window, std::move(definition), {.protocol = "SSM", .lower_panel_ids = {"rpm"}});
    int factoryCalls = 0;
    services.logging_engine.RegisterProtocol("SSM",
                                             [&factoryCalls](const fastecu::logging::LoggingRunSnapshot&)
                                             {
                                                 ++factoryCalls;
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->BlockPollUntilCancelled();
                                                 return protocol;
                                             });
    fastecu::testing::SignalRecorder errors{&window, &MainWindow::logE};
    QString dialogText;
    Qt::TextFormat dialogFormat = Qt::AutoText;
    QTimer timer;
    timer.setInterval(5);
    QObject::connect(&timer, &QTimer::timeout, &window,
                     [&]
                     {
                         for (auto *widget : QApplication::topLevelWidgets())
                         {
                             if (auto *notice = qobject_cast<QMessageBox *>(widget);
                                 notice != nullptr && notice->isVisible())
                             {
                                 dialogText = notice->text();
                                 dialogFormat = notice->textFormat();
                                 notice->accept();
                             }
                         }
                     });
    timer.start();
    window.continueStartLogging();
    timer.stop();
    const auto lines = logLines(errors);
    EXPECT_THAT(dialogText.toStdString(),
                ::testing::AllOf(::testing::HasSubstr("SSM"), ::testing::HasSubstr("rpm"),
                                 ::testing::HasSubstr("format"), ::testing::HasSubstr("<b>bad</b>"),
                                 ::testing::HasSubstr("expected")));
    EXPECT_EQ(dialogFormat, Qt::PlainText);
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_THAT(std::get<0>(lines.front()),
                ::testing::AllOf(::testing::HasSubstr("SSM"), ::testing::HasSubstr("rpm"),
                                 ::testing::HasSubstr("format"), ::testing::HasSubstr("<b>bad</b>"),
                                 ::testing::HasSubstr("expected")));
    EXPECT_EQ(factoryCalls, 0);
    EXPECT_FALSE(services.logging_engine.IsRunning());
    EXPECT_THAT(window.logging_run_->view().active_snapshot, Eq(std::nullopt));
    EXPECT_FALSE(action->isChecked());
    EXPECT_FALSE(window.logging_state_);
}

TEST_F(MainWindowTest, loggingDefinitionErrorShowsContextAndLeavesStopped)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingDefinitionErrorShowsContextAndLeavesStopped());
}

void MainWindowTest::checkLoggingCapabilityChecksumIsNotSupportEvidence()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    prepareLogging(window, "SSM");
    auto rpm = window.logger_model_->Definition().parameters.front();
    rpm.ecu_byte_index = "6";
    rpm.ecu_bit = "1";
    installLoggingFixture(window, {.parameters = {rpm}}, {.protocol = "SSM", .lower_panel_ids = {"rpm"}});
    // Five calibration-ID bytes plus one feature byte. 0x86 is checksum, not capability byte 6.
    window.parseLogValueList(frame({0x80, 0xf0, 0x10, 7, 0xff, 0, 0, 0, 0, 0, 0, 0x86}), "SSM");
    EXPECT_EQ(window.logger_model_->ParameterSupport("SSM", "rpm"), fastecu::logging::EcuSupport::kUnknown);
    driver.stop();
}
TEST_F(MainWindowTest, loggingCapabilityChecksumIsNotSupportEvidence)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingCapabilityChecksumIsNotSupportEvidence());
}

void MainWindowTest::checkLoggingSwitchOnlyDefaultsAreSelectable()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    prepareLogging(window, "SSM");
    window.config_session_->Settings().selected_log_protocol = "SSM";
    installLoggingFixture(
        window,
        {.switches =
             {{.protocol = "SSM", .id = "flag", .name = "Flag", .address = "20", .enabled = true, .sample_bit = "3"}}},
        {.protocol = "SSM"});
    window.ecuid_ = "SWITCH_ONLY_TARGET";
    ASSERT_TRUE(writeTextFile(QString::fromStdString(window.config_session_->EffectivePaths().logger_file),
                              "<config><logger/></config>"));
    window.loadLoggerSelection();
    ASSERT_EQ(window.logger_model_->Selection().switch_ids, (std::vector<std::string>{"flag"}));
    ASSERT_EQ(window.logger_model_->Selection().protocol, "SSM");
    services.logging_engine.RegisterProtocol("SSM",
                                             [](const auto&)
                                             {
                                                 auto source = std::make_unique<ScriptedLoggingProtocol>();
                                                 source->BlockPollUntilCancelled();
                                                 return source;
                                             });
    window.continueStartLogging();
    ASSERT_THAT(window.logging_run_->view().active_snapshot, Optional(_));
    ASSERT_EQ(window.logging_run_->view().active_snapshot->Session().Channels().size(), 1U);
    emit services.logging_engine.valuesUpdated({{.channel_id = "switch:flag", .numeric_value = 1}});
    EXPECT_EQ(window.logging_run_->view().values.SwitchValue("SSM", "flag"), "1");
    services.logging_engine.Stop();
    driver.stop();
}
TEST_F(MainWindowTest, loggingSwitchOnlyDefaultsAreSelectable)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingSwitchOnlyDefaultsAreSelectable());
}

void MainWindowTest::checkLoggingUnknownMutSelectionNeedsNoRomMatch()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    driver.stop();
    prepareLogging(window, "MUT_DMA");
    window.config_session_->Settings().selected_log_protocol = "MUT_DMA";
    EXPECT_EQ(window.logger_model_->ParameterSupport("MUT_DMA", "rpm"), fastecu::logging::EcuSupport::kUnknown);
    bool offered = false;
    QTimer::singleShot(0, &window,
                       [&]
                       {
                           auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                           ASSERT_NE(dialog, nullptr);
                           auto *combo = dialog->findChild<QComboBox *>("Digital value 0");
                           ASSERT_NE(combo, nullptr);
                           offered = combo->findData(QStringList{"MUT_DMA", "rpm"}) >= 0;
                           dialog->accept();
                       });
    window.changeLogValues(1, "MUT_DMA");
    EXPECT_TRUE(offered);
    services.logging_engine.RegisterProtocol("MUT_DMA",
                                             [](const auto&)
                                             {
                                                 auto source = std::make_unique<ScriptedLoggingProtocol>();
                                                 source->BlockPollUntilCancelled();
                                                 return source;
                                             });
    window.continueStartLogging();
    EXPECT_TRUE(services.logging_engine.IsRunning());
    ASSERT_THAT(window.logging_run_->view().active_snapshot, Optional(_));
    EXPECT_EQ(window.logging_run_->view().active_snapshot->Session().Channels().size(), 1U);
    services.logging_engine.Stop();
}
TEST_F(MainWindowTest, loggingUnknownMutSelectionNeedsNoRomMatch)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingUnknownMutSelectionNeedsNoRomMatch());
}

void MainWindowTest::checkLoggingSelectionEditsWaitForNextRun()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    driver.stop();
    prepareLogging(window, "SSM");
    window.config_session_->Settings().selected_log_protocol = "SSM";
    window.ecuid_ = "PENDING_SELECTION";
    auto rpm = window.logger_model_->Definition().parameters.front();
    auto temperature = rpm;
    temperature.id = "temperature";
    temperature.name = "Temperature";
    installLoggingFixture(window, {.parameters = {rpm, temperature}}, {.protocol = "SSM", .lower_panel_ids = {"rpm"}});
    services.logging_engine.RegisterProtocol("SSM",
                                             [](const auto&)
                                             {
                                                 auto source = std::make_unique<ScriptedLoggingProtocol>();
                                                 source->BlockPollUntilCancelled();
                                                 return source;
                                             });
    window.continueStartLogging();
    window.updateLogboxes("SSM");
    QTimer::singleShot(0, &window,
                       [&]
                       {
                           auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                           ASSERT_NE(dialog, nullptr);
                           auto *combo = dialog->findChild<QComboBox *>("Digital value 0");
                           ASSERT_NE(combo, nullptr);
                           combo->setCurrentIndex(combo->findData(QStringList{"SSM", "temperature"}));
                           dialog->accept();
                       });
    window.changeLogValues(1, "SSM");
    ASSERT_EQ(window.logger_model_->Selection().lower_panel_ids, (std::vector<std::string>{"temperature"}));
    ASSERT_THAT(window.logging_run_->view().active_snapshot, Optional(_));
    EXPECT_EQ(window.logging_run_->view().active_snapshot->Selection().lower_panel_ids,
              (std::vector<std::string>{"rpm"}));
    auto *box = window.findChild<QGroupBox *>("valueGroupBox0");
    ASSERT_NE(box, nullptr);
    EXPECT_EQ(box->title(), "rpm");
    auto *pending = window.findChild<QLabel *>("logging_pending_label");
    ASSERT_NE(pending, nullptr);
    EXPECT_FALSE(pending->isHidden());
    EXPECT_TRUE(pending->text().contains("restart", Qt::CaseInsensitive));
    const auto stored = services.logger_definitions.LoadSelection(window.config_session_->EffectivePaths().logger_file,
                                                                  "PENDING_SELECTION");
    ASSERT_THAT(stored, fastecu::testing::IsOk());
    ASSERT_THAT(*stored, Optional(_));
    EXPECT_EQ((**stored).lower_panel_ids, (std::vector<std::string>{"temperature"}));
    services.logging_engine.Stop();
    window.continueStartLogging();
    ASSERT_THAT(window.logging_run_->view().active_snapshot, Optional(_));
    EXPECT_EQ(window.logging_run_->view().active_snapshot->Selection().lower_panel_ids,
              (std::vector<std::string>{"temperature"}));
    EXPECT_TRUE(pending->isHidden());
    services.logging_engine.Stop();
}
TEST_F(MainWindowTest, loggingSelectionEditsWaitForNextRun)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingSelectionEditsWaitForNextRun());
}

void MainWindowTest::checkLoggingSwitchValuesAndLiteralUnits()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    driver.stop();
    prepareLogging(window, "SSM");
    window.config_session_->Settings().selected_log_protocol = "SSM";
    auto rpm = window.logger_model_->Definition().parameters.front();
    rpm.name = "<b>RPM</b>";
    rpm.conversions.front().units = "<b>unit</b>";
    installLoggingFixture(
        window,
        {.parameters = {rpm},
         .switches = {{.protocol = "SSM",
                       .id = "rpm",
                       .name = "<b>Flag</b>",
                       .address = "20",
                       .ecu_byte_index = "0",
                       .ecu_bit = "1",
                       .sample_bit = "7"}}},
        {.protocol = "SSM", .gauge_ids = {"rpm"}, .lower_panel_ids = {"rpm", "rpm"}, .switch_ids = {"rpm"}});
    services.logging_engine.RegisterProtocol("SSM",
                                             [](const auto&)
                                             {
                                                 auto source = std::make_unique<ScriptedLoggingProtocol>();
                                                 source->BlockPollUntilCancelled();
                                                 return source;
                                             });
    window.continueStartLogging();
    window.updateLogboxes("SSM");
    auto *flag = window.findChild<QLabel *>("switch_label0");
    ASSERT_NE(flag, nullptr);
    EXPECT_TRUE(flag->text().contains("Unavailable", Qt::CaseInsensitive));
    emit services.logging_engine.valuesUpdated(
        {{.channel_id = "parameter:rpm", .numeric_value = 42}, {.channel_id = "switch:rpm", .numeric_value = 1}});
    EXPECT_TRUE(flag->text().contains("ON"));
    EXPECT_EQ(flag->textFormat(), Qt::PlainText);
    EXPECT_EQ(window.logging_run_->view().values.SwitchValue("SSM", "rpm"), "1");
    EXPECT_EQ(window.logging_run_->view().values.ParameterValue("SSM", "rpm"), "42");
    for (int i : {0, 1})
    {
        SCOPED_TRACE(::testing::PrintToString(i));
        auto *value = window.findChild<QLabel *>("log_label" + QString::number(i));
        ASSERT_NE(value, nullptr);
        EXPECT_EQ(value->text(), "42 <b>unit</b>");
        EXPECT_EQ(value->textFormat(), Qt::PlainText);
    }
    emit services.logging_engine.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = std::nullopt}});
    auto *unavailable = window.findChild<QLabel *>("log_label0");
    ASSERT_NE(unavailable, nullptr);
    EXPECT_EQ(unavailable->text(), "Unavailable <b>unit</b>");
    EXPECT_TRUE(flag->text().contains("ON"));
    emit services.logging_engine.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 7}});
    EXPECT_EQ(unavailable->text(), "7 <b>unit</b>");
    emit services.logging_engine.valuesUpdated({{.channel_id = "switch:rpm", .numeric_value = 0}});
    EXPECT_TRUE(flag->text().contains("OFF"));
    services.logging_engine.Stop();
    window.logger_model_->SetSelection({.protocol = "SSM", .gauge_ids = {"rpm"}});
    window.continueStartLogging();
    EXPECT_TRUE(window.logging_run_->view().values.ParameterValue("SSM", "rpm").isEmpty());
    emit services.logging_engine.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 9}});
    EXPECT_EQ(window.logging_run_->view().values.ParameterValue("SSM", "rpm"), "9");
    services.logging_engine.Stop();
}
TEST_F(MainWindowTest, loggingSwitchValuesAndLiteralUnits)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingSwitchValuesAndLiteralUnits());
}

void MainWindowTest::checkLoggingUnavailableSelectionCanBeReplaced(bool knownUnsupported)
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    driver.stop();
    prepareLogging(window, "SSM");
    window.config_session_->Settings().selected_log_protocol = "SSM";
    auto rpm = window.logger_model_->Definition().parameters.front();
    auto replacement = rpm;
    replacement.id = "replacement";
    const std::string selected = knownUnsupported ? "rpm" : "missing";
    installLoggingFixture(window, {.parameters = {rpm, replacement}},
                          {.protocol = "SSM", .lower_panel_ids = {selected}});
    if (knownUnsupported)
    {
        window.logger_model_->SetParameterSupport("SSM", "rpm", fastecu::logging::EcuSupport::kUnsupported);
    }
    window.updateLogboxes("SSM");
    EXPECT_EQ(window.ui_->logBoxLayout->count(), 1);
    int factories = 0;
    services.logging_engine.RegisterProtocol("SSM",
                                             [&](const auto&)
                                             {
                                                 ++factories;
                                                 auto source = std::make_unique<ScriptedLoggingProtocol>();
                                                 source->BlockPollUntilCancelled();
                                                 return source;
                                             });
    fastecu::testing::SignalRecorder errors{&window, &MainWindow::logE};
    driver.start();
    window.continueStartLogging();
    driver.stop();
    EXPECT_EQ(factories, 0);
    EXPECT_THAT(window.logging_run_->view().active_snapshot, Eq(std::nullopt));
    const auto lines = logLines(errors);
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_THAT(std::get<0>(lines.front()),
                ::testing::AllOf(::testing::HasSubstr("SSM"), ::testing::HasSubstr(selected)));
    bool inspected = false;
    QTimer::singleShot(0, &window,
                       [&]
                       {
                           auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                           ASSERT_NE(dialog, nullptr);
                           auto *combo = dialog->findChild<QComboBox *>("Digital value 0");
                           ASSERT_NE(combo, nullptr);
                           EXPECT_EQ(combo->currentData().toStringList(),
                                     (QStringList{"SSM", QString::fromStdString(selected)}));
                           EXPECT_TRUE(combo->currentText().contains("unavailable", Qt::CaseInsensitive));
                           combo->setCurrentIndex(combo->findData(QStringList{"SSM", "replacement"}));
                           inspected = true;
                           dialog->accept();
                       });
    window.changeLogValues(1, "SSM");
    EXPECT_TRUE(inspected);
    EXPECT_EQ(window.logger_model_->Selection().lower_panel_ids, (std::vector<std::string>{"replacement"}));
}
TEST_F(MainWindowTest, loggingUnavailableSelectionCanBeReplaced)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingUnavailableSelectionCanBeReplaced(false));
}
TEST_F(MainWindowTest, loggingUnsupportedSelectionCanBeReplaced)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingUnavailableSelectionCanBeReplaced(true));
}

void MainWindowTest::checkLoggingDisplayErrorContinuesOtherSamples()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    constructorDriver.stop();
    prepareLogging(window, "SSM");
    window.config_session_->Settings().selected_log_protocol = "SSM";
    services.logging_engine.RegisterProtocol("SSM",
                                             [](const fastecu::logging::LoggingRunSnapshot&)
                                             {
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->BlockPollUntilCancelled();
                                                 return protocol;
                                             });
    window.continueStartLogging();
    ASSERT_TRUE(services.logging_engine.IsRunning());
    fastecu::testing::SignalRecorder errors{&window, &MainWindow::logE};
    emit services.logging_engine.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 42}});
    EXPECT_EQ(window.logging_run_->view().values.ParameterValue("SSM", "rpm"), "42");
    EXPECT_EQ(errors.Snapshot().size(), 1U);
    EXPECT_TRUE(services.logging_engine.IsRunning());
    services.logging_engine.Stop();
}

void MainWindowTest::checkReplacementDuringSampleNotificationDiscardsOldBatch()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    driver.stop();
    prepareLogging(window, "SSM");
    window.config_session_->Settings().selected_log_protocol = "SSM";
    services.logging_engine.RegisterProtocol("SSM",
                                             [](const auto&)
                                             {
                                                 auto source = std::make_unique<ScriptedLoggingProtocol>();
                                                 source->BlockPollUntilCancelled();
                                                 return source;
                                             });
    window.continueStartLogging();
    bool replaced = false;
    const auto subscription = QObject::connect(
        &window, &MainWindow::logE, &window,
        [&](const QString&, bool, bool)
        {
            if (replaced)
            {
                return;
            }
            replaced = true;
            window.ui_->actionToggleRealtime->setChecked(false);
            window.toggleRealtime();
            window.continueStartLogging();
        },
        Qt::DirectConnection);
    emit services.logging_engine.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 99}});
    EXPECT_TRUE(replaced);
    EXPECT_TRUE(services.logging_engine.IsRunning());
    EXPECT_TRUE(window.logging_run_->view().values.ParameterValue("SSM", "rpm").isEmpty());
    emit services.logging_engine.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 43}});
    EXPECT_EQ(window.logging_run_->view().values.ParameterValue("SSM", "rpm"), "43");
    QObject::disconnect(subscription);
    services.logging_engine.Stop();
}

void MainWindowTest::checkQueuedStopDuringRenderingPreventsTheCsvRow()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    driver.stop();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    window.config_session_->Settings().datalog_files_directory = directory.path().toStdString();
    prepareLogging(window, "SSM");
    window.config_session_->Settings().selected_log_protocol = "SSM";
    window.logging_run_->setCsvEnabled(true, directory.path(), "run");
    services.logging_engine.RegisterProtocol("SSM",
                                             [](const auto&)
                                             {
                                                 auto source = std::make_unique<ScriptedLoggingProtocol>();
                                                 source->BlockPollUntilCancelled();
                                                 return source;
                                             });
    window.continueStartLogging();
    const auto csvPath = window.logging_run_->view().csv_path;
    QTimer::singleShot(0, &window,
                       [&]
                       {
                           window.ui_->actionToggleRealtime->setChecked(false);
                           window.toggleRealtime();
                       });
    emit services.logging_engine.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    EXPECT_FALSE(services.logging_engine.IsRunning());
    QFile csv{csvPath};
    ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
    EXPECT_EQ(csv.readAll(), "Time,rpm\n");
}

TEST_F(MainWindowTest, loggingDisplayErrorContinuesOtherSamples)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingDisplayErrorContinuesOtherSamples());
}

void MainWindowTest::checkLoggingUsesTheSessionLogProtocol()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;
    prepareLogging(window, "CDBG");
    services.logging_engine.RegisterProtocol("CDBG",
                                             [](const fastecu::logging::LoggingRunSnapshot&)
                                             {
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->BlockPollUntilCancelled();
                                                 return protocol;
                                             });
    window.config_session_->Settings().selected_log_protocol = "CDBG";
    window.protocol_ = "STALE_SSM_FILTER";

    ModalDriver driver{QString()};
    driver.start();
    window.continueStartLogging();
    driver.stop();
    const auto view = window.logging_run_->view();
    ASSERT_THAT(view.active_snapshot, Optional(_));
    EXPECT_EQ(view.active_snapshot->ProtocolKey(), "CDBG");
    EXPECT_EQ(view.active_snapshot->Session().Protocol(), fastecu::logging::LoggingProtocolId::kCdbg);
    services.logging_engine.Stop();
}

TEST_F(MainWindowTest, loggingUsesTheSessionLogProtocol)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingUsesTheSessionLogProtocol());
}

void MainWindowTest::checkSelectedSerialPortIsEmptyWithoutPorts()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    MainWindow& window = *under.window;
    window.serial_ports_.clear();
    window.serial_port_list_->clear();
    ASSERT_EQ(window.selectedSerialPort(), QString());
    window.serial_ports_ = {"ttyUSB0"};
    window.serial_port_list_->addItem("ttyUSB0");
    ASSERT_EQ(window.selectedSerialPort(), QString("ttyUSB0"));
}

TEST_F(MainWindowTest, selectedSerialPortIsEmptyWithoutPorts)
{
    ASSERT_NO_FATAL_FAILURE(checkSelectedSerialPortIsEmptyWithoutPorts());
}

void MainWindowTest::checkDtcWindowWithoutAPortWarnsInsteadOfCrashing()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;
    window.serial_ports_.clear();
    window.serial_port_list_->clear();
    EXPECT_CALL(*services.fake, SetSerialPortList(::testing::_)).Times(0);
    ModalDriver driver{QString()};
    driver.start();
    for (const ActionName& command : {kDtcWindow, kBiuCommunication, kTerminal})
    {
        SCOPED_TRACE(::testing::PrintToString(command));
        ASSERT_TRUE(triggerMenu(window, command));
    }
    driver.stop();
    ASSERT_TRUE(!driver.timedOut());
}

TEST_F(MainWindowTest, dtcWindowWithoutAPortWarnsInsteadOfCrashing)
{
    ASSERT_NO_FATAL_FAILURE(checkDtcWindowWithoutAPortWarnsInsteadOfCrashing());
}

void MainWindowTest::checkRepeatedSaveFailuresLogOnceUntilASuccess()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    TestServices services{root.path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    // A fresh root has no saved vehicle; the startup gate would ask for one.
    ASSERT_THAT(services.config.SelectRow(0), fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    fastecu::testing::SignalRecorder errors{&window, &MainWindow::logE};
    const QString configFile = QString::fromStdString(services.config.ProvisionedPaths().config_file);
    ASSERT_TRUE(QFile::remove(configFile));
    ASSERT_TRUE(QDir().mkpath(configFile));

    services.config.Settings().toolbar_iconsize = "48";
    window.saveSettings();
    window.saveSettings();
    window.saveSettings();
    ASSERT_EQ(errors.Count(), 1U);
    ASSERT_TRUE(std::get<0>(errors.Snapshot().front()).contains(configFile));
    ASSERT_EQ(services.config.Settings().toolbar_iconsize, std::string("48"));

    ASSERT_TRUE(QDir().rmdir(configFile));
    window.saveSettings();
    ASSERT_EQ(errors.Count(), 1U);
    QFile saved{configFile};
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    ASSERT_TRUE(saved.readAll().contains(R"(data="48")"));
    saved.close();
    ASSERT_TRUE(QFile::remove(configFile));
    ASSERT_TRUE(QDir().mkpath(configFile));
    window.saveSettings();
    ASSERT_EQ(errors.Count(), 2U);
}

TEST_F(MainWindowTest, repeatedSaveFailuresLogOnceUntilASuccess)
{
    ASSERT_NO_FATAL_FAILURE(checkRepeatedSaveFailuresLogOnceUntilASuccess());
}

void MainWindowTest::checkBiuWindowRemembersTheOpenedPort()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    ON_CALL(*services.fake, GetOpenedSerialPort()).WillByDefault(::testing::Return(QString("ttyUSB0")));
    window.previous_serial_port_.clear();
    window.config_session_->Settings().serial_port = "none";
    window.saveSettings();
    const QString configFile =
        config_root_->path() + "/" + QString::fromStdString(kTestApplication.version) + "/config/fastecu.cfg";

    ModalDriver driver{QString()};
    driver.start();
    ASSERT_TRUE(triggerMenu(window, kBiuCommunication));
    driver.stop();

    // As open_serial_port did for the legacy BIU path: the chosen port is
    // remembered for the next launch and as the previously opened port.
    ASSERT_EQ(window.previous_serial_port_, QString("ttyUSB0"));
    ASSERT_EQ(window.config_session_->Settings().serial_port, std::string("ttyUSB0"));
    QFile saved{configFile};
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    ASSERT_TRUE(saved.readAll().contains(R"(data="ttyUSB0")"));
}

TEST_F(MainWindowTest, biuWindowRemembersTheOpenedPort)
{
    ASSERT_NO_FATAL_FAILURE(checkBiuWindowRemembersTheOpenedPort());
}

void MainWindowTest::checkDisconnectReturnsTheAdapterToIdle()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;
    {
        ::testing::InSequence order;
        EXPECT_CALL(*services.fake, ResetConnection());
        EXPECT_CALL(*services.fake, SetSerialPortBaudrate(QString("4800")));
        EXPECT_CALL(*services.fake, SetSerialPortParity(0));
    }
    EXPECT_CALL(*services.fake, SetIsCanConnection(::testing::_)).Times(0);
    ASSERT_TRUE(triggerMenu(window, kDisconnectFromEcu));
    // Check now, so facade teardown cannot over-saturate the expectations.
    ASSERT_TRUE(::testing::Mock::VerifyAndClearExpectations(services.fake));
    ASSERT_TRUE(window.serial_port_list_->isEnabled());
}

TEST_F(MainWindowTest, disconnectReturnsTheAdapterToIdle)
{
    ASSERT_NO_FATAL_FAILURE(checkDisconnectReturnsTheAdapterToIdle());
}

void MainWindowTest::checkConnectOnAnotherMakeDisconnectsWithoutIdentifying()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Mitsubishi", "K-Line"));
    EXPECT_CALL(*services.fake, WriteSerialDataEchoCheck(::testing::_)).Times(0);
    EXPECT_CALL(*services.fake, SetSerialPortParity(0)).Times(::testing::AtLeast(1));

    QElapsedTimer elapsed;
    elapsed.start();
    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(elapsed.elapsed() < 1000); // the legacy loop waited 2.5 s here
    ASSERT_TRUE(!window.connection_coordinator_->identifying());
    ASSERT_TRUE(!window.ecu_init_complete_);
    ASSERT_TRUE(window.serial_port_list_->isEnabled());
}

TEST_F(MainWindowTest, connectOnAnotherMakeDisconnectsWithoutIdentifying)
{
    ASSERT_NO_FATAL_FAILURE(checkConnectOnAnotherMakeDisconnectsWithoutIdentifying());
}

void MainWindowTest::checkSubaruKlineConnectIdentifiesOffTheUiThread()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    std::atomic<bool> readOffUiThread = false;
    EXPECT_CALL(*services.fake, ReadSerialData(::testing::_))
        .WillOnce(
            [&window, &readOffUiThread](std::uint16_t)
            {
                readOffUiThread.store(QThread::currentThread() != window.thread());
                return kEcuInit;
            })
        .WillRepeatedly(::testing::Return(QByteArray{}));

    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_TRUE(!window.log_transport_list_->isEnabled());
    ASSERT_TRUE(!window.serial_port_list_->isEnabled());

    constructorDriver.start();
    ASSERT_TRUE(fastecu::testing::WaitUntil([&] { return !window.connection_coordinator_->identifying(); },
                                            std::chrono::milliseconds(5000)));
    constructorDriver.stop();
    ASSERT_TRUE(window.ecu_init_complete_);
    ASSERT_EQ(window.ecuid_, QString("3152584006"));
    ASSERT_TRUE(readOffUiThread.load());
    ASSERT_TRUE(!window.connection_coordinator_->identifying());
    ASSERT_TRUE(window.log_transport_list_->isEnabled());
    ASSERT_TRUE(!window.serial_port_list_->isEnabled()); // stays locked while connected, as before
}

TEST_F(MainWindowTest, subaruKlineConnectIdentifiesOffTheUiThread)
{
    ASSERT_NO_FATAL_FAILURE(checkSubaruKlineConnectIdentifiesOffTheUiThread());
}

void MainWindowTest::checkSubaruConnectThatNeverAnswersDisconnectsAndRestoresControls()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    ASSERT_TRUE(services.fake != nullptr);
    // Virtual time: the retry delays and the identification deadline pass instantly.
    services.make_clock = []() -> std::unique_ptr<fastecu::IClock>
    {
        auto clock = std::make_unique<fastecu::FakeClock>();
        clock->SetNowAutoAdvance(std::chrono::milliseconds{10});
        return clock;
    };
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));

    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(fastecu::testing::WaitUntil([&] { return !window.connection_coordinator_->identifying(); },
                                            std::chrono::milliseconds(15000)));
    ASSERT_TRUE(!window.ecu_init_complete_);
    ASSERT_TRUE(window.log_transport_list_->isEnabled());
    ASSERT_TRUE(window.serial_port_list_->isEnabled());
    ASSERT_TRUE(window.ecu_radio_button_->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button_->isEnabled());
}

TEST_F(MainWindowTest, subaruConnectThatNeverAnswersDisconnectsAndRestoresControls)
{
    ASSERT_NO_FATAL_FAILURE(checkSubaruConnectThatNeverAnswersDisconnectsAndRestoresControls());
}

void MainWindowTest::checkDisconnectDuringIdentificationCancelsAndDropsTheResult()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));

    EXPECT_CALL(*services.fake, ReadSerialData(::testing::_)).WillOnce(::testing::Return(kEcuInit));
    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    // Leave a successful completion queued on the UI thread before cancelling.
    ASSERT_TRUE(window.identify_launcher_->waitForWorker(std::chrono::milliseconds(5000)));
    ASSERT_TRUE(triggerMenu(window, kDisconnectFromEcu));
    ASSERT_TRUE(!window.connection_coordinator_->identifying());
    ASSERT_TRUE(window.log_transport_list_->isEnabled());
    ASSERT_TRUE(window.serial_port_list_->isEnabled());
    ASSERT_TRUE(window.ecu_radio_button_->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button_->isEnabled());
    fastecu::testing::ProcessEventsFor(std::chrono::milliseconds(200)); // any completion already queued must be dropped
    ASSERT_TRUE(!window.ecu_init_complete_);
}

TEST_F(MainWindowTest, disconnectDuringIdentificationCancelsAndDropsTheResult)
{
    ASSERT_NO_FATAL_FAILURE(checkDisconnectDuringIdentificationCancelsAndDropsTheResult());
}

void MainWindowTest::checkLoggingSelectionWiringAndSupportPreservation()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    window.vbatt_timer_->stop();
    const auto cfg = QString::fromStdString(services.config.EffectivePaths().logger_file);
    window.ecuid_ = "MODEL_TEST";
    installLoggingFixture(
        window,
        {.parameters = {{.protocol = "SSM", .id = "rpm", .ecu_byte_index = "0", .ecu_bit = "0", .enabled = true}},
         .switches = {{.protocol = "SSM", .id = "flag", .ecu_byte_index = "5", .enabled = true}}},
        {.protocol = "SSM", .gauge_ids = {"old"}, .lower_panel_ids = {"old"}, .switch_ids = {"old"}});
    // Restore and save policy is owned by LoggingSelectionUseCase's tests; this
    // pins that the window reaches it and that restoring keeps ECU support.
    window.logger_model_->SetParameterSupport("SSM", "rpm", fastecu::logging::EcuSupport::kUnsupported);
    ASSERT_TRUE(writeTextFile(
        cfg,
        R"(<config><logger><ecu id="MODEL_TEST"><protocol id="SSM"><parameters><gauges><parameter id="unknown"/></gauges><lower_panel><parameter id="rpm"/></lower_panel></parameters><switches><switch id="flag"/></switches></protocol></ecu></logger></config>)"));
    window.loadLoggerSelection();
    ASSERT_EQ(window.logger_model_->Selection().gauge_ids, (std::vector<std::string>{"unknown"}));
    ASSERT_TRUE(!window.logger_model_->ParameterAvailable("SSM", "rpm"));
    // A valid capability byte updates parameters; missing switch bytes retain flags.
    window.parseLogValueList(frame({0x80, 0xf0, 0x10, 2, 0xff, 1, 0x82}), "SSM");
    ASSERT_TRUE(window.logger_model_->ParameterAvailable("SSM", "rpm"));
    ASSERT_TRUE(window.logger_model_->SwitchAvailable("SSM", "flag"));
    ASSERT_TRUE(window.logger_model_->Definition().parameters.front().enabled);
    window.savePendingLoggingSelection(window.logger_model_->Selection());
    const auto stored = services.logger_definitions.LoadSelection(cfg.toStdString(), "MODEL_TEST");
    ASSERT_THAT(stored, fastecu::testing::IsOk());
    ASSERT_THAT(*stored, Optional(_));
    ASSERT_TRUE(**stored == window.logger_model_->Selection());
    auto withCustomProtocol = window.logger_model_->Definition();
    withCustomProtocol.parameters.push_back({.protocol = "CUSTOM", .id = "other", .enabled = true});
    const auto retainedSelection = window.logger_model_->Selection();
    installLoggingFixture(window, std::move(withCustomProtocol), retainedSelection);
    window.logger_model_->SetParameterSupport("CUSTOM", "other", fastecu::logging::EcuSupport::kUnsupported);
    window.logger_model_->SetParameterSupport("SSM", "rpm", fastecu::logging::EcuSupport::kUnsupported);
    window.connection_presentation_.identified({.ecu_id = "NEXT_TARGET"});
    ASSERT_EQ(window.logger_model_->ParameterSupport("SSM", "rpm"), fastecu::logging::EcuSupport::kUnknown);
    ASSERT_EQ(window.logger_model_->SwitchSupport("SSM", "flag"), fastecu::logging::EcuSupport::kUnknown);
    EXPECT_EQ(window.logger_model_->ParameterSupport("CUSTOM", "other"), fastecu::logging::EcuSupport::kUnknown);
    driver.stop();
}

TEST_F(MainWindowTest, loggingSelectionWiringAndSupportPreservation)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingSelectionWiringAndSupportPreservation());
}

// Builds the log transport list for `protocol` from the saved settings, as
// startup does, and checks the list restores the saved log protocol.
void MainWindowTest::checkLogTransportListRestoresTheSavedLogProtocol(const QString& protocol,
                                                                      const QString& flashTransport,
                                                                      const QString& logTransport,
                                                                      const QString& logProtocol, int expectedIndex)
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    driver.stop();
    window.vbatt_timer_->stop();
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, protocol));
    auto& settings = window.config_session_->Settings();
    settings.selected_flash_transport = flashTransport.toStdString();
    settings.selected_log_transport = logTransport.toStdString();
    settings.selected_log_protocol = logProtocol.toStdString();
    window.protocol_.clear();
    const QSignalBlocker blocker(window.log_transport_list_);

    window.createLogTransportsList();

    EXPECT_EQ(window.protocol_, logProtocol);
    EXPECT_EQ(window.log_transport_list_->currentIndex(), expectedIndex);
}

TEST_F(MainWindowTest, logTransportListRestoresTheLogProtocolWhenFlashAndLogTransportsDiffer)
{
    ASSERT_NO_FATAL_FAILURE(
        checkLogTransportListRestoresTheSavedLogProtocol("sub_ecu_denso_sh7058_can", "iso15765", "K-Line", "SSM", 0));
}

TEST_F(MainWindowTest, logTransportListRestoresANonSsmLogProtocol)
{
    ASSERT_NO_FATAL_FAILURE(
        checkLogTransportListRestoresTheSavedLogProtocol("sub_ecu_denso_sh7058", "K-Line", "K-Line", "MUT_DMA", 0));
}

TEST_F(MainWindowTest, logTransportListFallsBackToTheFirstTransportForAStaleSavedTransport)
{
    ASSERT_NO_FATAL_FAILURE(checkLogTransportListRestoresTheSavedLogProtocol("sub_ecu_denso_sh7058_can", "iso15765",
                                                                             "NoSuchTransport", "SSM", 0));
}

TEST_F(MainWindowTest, logTransportListRestoresTheSecondOfTwoSavedTransports)
{
    ASSERT_NO_FATAL_FAILURE(
        checkLogTransportListRestoresTheSavedLogProtocol("sub_ecu_test_two_log_transports", "K-Line", "CAN", "SSM", 1));
}

void MainWindowTest::checkVehicleChangeKeepsTheSavedLogProtocol()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    window.vbatt_timer_->stop();
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    window.protocol_.clear();

    // SelectRow supplies the CDBG row's log protocol; rebuilding the list after
    // the change also fires logTransportChanged.
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_test_cdbg"));
    window.selectVehicleFinished(QDialog::Accepted);
    driver.stop();

    EXPECT_EQ(window.protocol_, "CDBG");
}

TEST_F(MainWindowTest, vehicleChangeKeepsTheSavedLogProtocol)
{
    ASSERT_NO_FATAL_FAILURE(checkVehicleChangeKeepsTheSavedLogProtocol());
}

// Before the fix, startup left protocol_ empty here, and pressing Logging was
// refused with "SSM logging protocol filter is empty" until the operator
// touched the Log combo.
void MainWindowTest::checkSsmLoggingStartsAfterStartupWhenFlashAndLogTransportsDiffer()
{
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can"));
    auto& settings = window.config_session_->Settings();
    settings.selected_flash_transport = "iso15765";
    settings.selected_log_transport = "K-Line";
    settings.selected_log_protocol = "SSM";
    QAction *action = prepareLogging(window, "SSM");
    // The constructor's state: nothing has set protocol_ yet.
    window.protocol_.clear();
    {
        const QSignalBlocker blocker(window.log_transport_list_);
        window.createLogTransportsList();
    }
    services.logging_engine.RegisterProtocol("SSM",
                                             [](const auto&)
                                             {
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->BlockPollUntilCancelled();
                                                 return protocol;
                                             });
    window.connection_presentation_.identified({.ecu_id = "ECU"});

    ModalDriver driver{QString()};
    driver.start();
    action->setChecked(false);
    ASSERT_TRUE(triggerMenu(window, kToggleRealtime));
    driver.stop();

    EXPECT_THAT(window.logging_run_->view().active_snapshot, Optional(_));
    EXPECT_TRUE(driver.acceptedTexts().filter("filter is empty").isEmpty())
        << qPrintable(driver.acceptedTexts().join(" | "));
    services.logging_engine.Stop();
}

TEST_F(MainWindowTest, ssmLoggingStartsAfterStartupWhenFlashAndLogTransportsDiffer)
{
    ASSERT_NO_FATAL_FAILURE(checkSsmLoggingStartsAfterStartupWhenFlashAndLogTransportsDiffer());
}

void MainWindowTest::checkSavingASelectionThatCannotBePersistedNoticesTheOperator()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    window.vbatt_timer_->stop();
    const auto cfg = QString::fromStdString(services.config.EffectivePaths().logger_file);
    window.ecuid_ = "MODEL_TEST";
    QFile::remove(cfg);
    QStringList notices;
    QObject::connect(&services.events, &QtEventSink::noticed, &window,
                     [&notices](const QString& message) { notices.append(message); });
    window.savePendingLoggingSelection(window.logger_model_->Selection());
    ASSERT_EQ(notices, QStringList{"Logger file: Unable to save logger config file '" + cfg + "'"});
    driver.stop();
}

TEST_F(MainWindowTest, savingASelectionThatCannotBePersistedNoticesTheOperator)
{
    ASSERT_NO_FATAL_FAILURE(checkSavingASelectionThatCannotBePersistedNoticesTheOperator());
}

void MainWindowTest::checkLoggingDefinitionFailureIsNonfatal()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    services.config.Settings().romraider_logger_definition_file = "/missing/logger.xml";
    MainWindow window{services.services()};
    ASSERT_THAT(window.logger_model_->Definition().parameters, IsEmpty());
    ASSERT_THAT(window.logger_model_->Selection().lower_panel_ids, IsEmpty());
    ASSERT_TRUE(window.ui_ != nullptr);
    driver.stop();
}

TEST_F(MainWindowTest, loggingDefinitionFailureIsNonfatal)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingDefinitionFailureIsNonfatal());
}

void MainWindowTest::checkUnresolvedDisplaySlotsRemainVisibleAndUpdateTheirOriginalLabels()
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
    window.updateLogboxes("SSM");
    ASSERT_EQ(window.ui_->logBoxLayout->count(), 3);
    window.updateLogboxValues("SSM");
    const auto *label = window.findChild<QLabel *>("log_label1");
    ASSERT_TRUE(label != nullptr);
    ASSERT_EQ(label->text(), QString("Unavailable rpm"));
    ASSERT_EQ(label->alignment(), Qt::Alignment(Qt::AlignRight));
    ASSERT_EQ(label->font().pointSize(), QGuiApplication::primaryScreen()->geometry().width() / 90);
}

TEST_F(MainWindowTest, unresolvedDisplaySlotsRemainVisibleAndUpdateTheirOriginalLabels)
{
    ASSERT_NO_FATAL_FAILURE(checkUnresolvedDisplaySlotsRemainVisibleAndUpdateTheirOriginalLabels());
}

struct ChooserDuplicateLabelIdentityCase
{
    std::string name;
    int tab;
    QString kind;
};
std::vector<ChooserDuplicateLabelIdentityCase> chooserDuplicateLabelIdentityRows()
{
    std::vector<ChooserDuplicateLabelIdentityCase> rows;

    rows.push_back(ChooserDuplicateLabelIdentityCase{"gauge", 0, QString("Gauge")});
    rows.push_back(ChooserDuplicateLabelIdentityCase{"digital", 1, QString("Digital")});
    rows.push_back(ChooserDuplicateLabelIdentityCase{"switch", 2, QString("Switch")});

    return rows;
}
class ChooserDuplicateLabelIdentityParameters : public MainWindowTest,
                                                public ::testing::WithParamInterface<ChooserDuplicateLabelIdentityCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, ChooserDuplicateLabelIdentityParameters,
                         ::testing::ValuesIn(chooserDuplicateLabelIdentityRows()),
                         [](const ::testing::TestParamInfo<ChooserDuplicateLabelIdentityCase>& paramInfo)
                         {
                             auto name = paramInfo.param.name;
                             for (char& c : name)
                             {
                                 if (!std::isalnum(static_cast<unsigned char>(c)))
                                 {
                                     c = '_';
                                 }
                             }
                             return name;
                         });
void MainWindowTest::checkChooserDuplicateLabelIdentity(int tab, QString kind)
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
    window.changeLogValues(tab, "SSM");
    ASSERT_TRUE(inspected);
    const auto& selected = window.logger_model_->Selection();
    ASSERT_EQ((tab == 0   ? selected.gauge_ids
               : tab == 1 ? selected.lower_panel_ids
                          : selected.switch_ids)
                  .at(0),
              std::string("first"));
    ASSERT_EQ(selected.protocol, std::string("SSM"));
}

TEST_P(ChooserDuplicateLabelIdentityParameters, chooserDuplicateLabelIdentity)
{
    ASSERT_NO_FATAL_FAILURE(checkChooserDuplicateLabelIdentity(GetParam().tab, GetParam().kind));
}

struct LoggingStartWaitsForIdentificationCase
{
    std::string name;
    bool target_is_ecu;
};
std::vector<LoggingStartWaitsForIdentificationCase> loggingStartWaitsForIdentificationRows()
{
    std::vector<LoggingStartWaitsForIdentificationCase> rows;

    rows.push_back(LoggingStartWaitsForIdentificationCase{"ECU", true});
    rows.push_back(LoggingStartWaitsForIdentificationCase{"TCU", false});

    return rows;
}
class LoggingStartWaitsForIdentificationParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<LoggingStartWaitsForIdentificationCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, LoggingStartWaitsForIdentificationParameters,
                         ::testing::ValuesIn(loggingStartWaitsForIdentificationRows()),
                         [](const ::testing::TestParamInfo<LoggingStartWaitsForIdentificationCase>& paramInfo)
                         {
                             auto name = paramInfo.param.name;
                             for (char& c : name)
                             {
                                 if (!std::isalnum(static_cast<unsigned char>(c)))
                                 {
                                     c = '_';
                                 }
                             }
                             return name;
                         });
void MainWindowTest::checkLoggingStartWaitsForIdentification(bool targetIsEcu)
{

    QSemaphore responseGate;
    WindowUnderTest under;
    ASSERT_NO_FATAL_FAILURE(createWindow(under));
    TestServices& services = *under.services;
    MainWindow& window = *under.window;
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "iso15765"));
    window.protocol_ = "SSM";
    (targetIsEcu ? window.ecu_radio_button_ : window.tcu_radio_button_)->setChecked(true);
    EXPECT_CALL(*services.fake,
                WriteSerialDataEchoCheck(frame({0x00, 0x00, 0x07, targetIsEcu ? 0xE0 : 0xE1, 0x22, 0xF1, 0x82})));
    EXPECT_CALL(*services.fake, ReadSerialData(::testing::_))
        .WillOnce(
            [&responseGate](std::uint16_t)
            {
                // Bound the wait so an assertion failure can still join the worker.
                responseGate.tryAcquire(1, 5000);
                return frame({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82, 0x12, 0x34, 0x56, 0x78, 0x9A});
            })
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
    bool targetFrozenInContinuation = false;
    services.logging_engine.RegisterProtocol(
        "SSM",
        [&window, &targetFrozenInContinuation](const fastecu::logging::LoggingRunSnapshot&)
        {
            targetFrozenInContinuation =
                !window.ecu_radio_button_->isEnabled() && !window.tcu_radio_button_->isEnabled();
            auto protocol = std::make_unique<ScriptedLoggingProtocol>();
            protocol->BlockPollUntilCancelled();
            return protocol;
        });

    // trigger() toggles a checkable action, as a click does: start unchecked
    // so the handler sees Logging switched on.
    action->setChecked(false);
    ASSERT_TRUE(triggerMenu(window, kToggleRealtime));
    ASSERT_TRUE(action->isChecked());
    ASSERT_THAT(window.logging_run_->view().active_snapshot, Eq(std::nullopt)); // still identifying
    (targetIsEcu ? window.tcu_radio_button_ : window.ecu_radio_button_)->click();
    responseGate.release();
    ASSERT_TRUE(fastecu::testing::WaitUntil([&] { return window.logging_run_->view().active_snapshot.has_value(); },
                                            std::chrono::milliseconds(5000)));
    ASSERT_EQ(window.ecuid_, QString("123456789A"));
    ASSERT_TRUE(window.logger_model_->ParameterAvailable("SSM", "rpm"));
    ASSERT_THAT(window.logging_run_->view().active_snapshot, Optional(_));
    ASSERT_EQ(window.logging_run_->view().active_snapshot->Target() == fastecu::logging::LoggingTarget::kEcu,
              targetIsEcu);
    ASSERT_TRUE(targetFrozenInContinuation);
    ASSERT_TRUE(window.ecu_radio_button_->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button_->isEnabled());
    services.logging_engine.Stop();
}

TEST_P(LoggingStartWaitsForIdentificationParameters, loggingStartWaitsForIdentification)
{
    ASSERT_NO_FATAL_FAILURE(checkLoggingStartWaitsForIdentification(GetParam().target_is_ecu));
}

void MainWindowTest::checkBatterySamplingDoesNotUseTheFacadeDuringIdentification()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    EXPECT_CALL(*services.fake, GetUseOpenport2Adapter()).Times(0);
    window.updateVbatt();
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_TRUE(::testing::Mock::VerifyAndClearExpectations(services.fake));
}

TEST_F(MainWindowTest, batterySamplingDoesNotUseTheFacadeDuringIdentification)
{
    ASSERT_NO_FATAL_FAILURE(checkBatterySamplingDoesNotUseTheFacadeDuringIdentification());
}

namespace
{

constexpr std::array<const char *, 4> kOperationsWindowNames{"EcuOperationsWindow", "BiuOperationsSubaruWindow",
                                                             "DtcOperationsWindow", "DataTerminalWindow"};

// Opens a stand-in operations window: a child dialog of `window` named
// `name`, with the log text edit and, when asked, the battery label.
QDialog *openOperationsWindow(MainWindow& window, const char *name, bool withBatteryLabel)
{
    auto *dialog = new QDialog(&window);
    dialog->setObjectName(QString::fromLatin1(name));
    auto *text = new QTextEdit(dialog);
    text->setObjectName("text_edit");
    if (withBatteryLabel)
    {
        auto *label = new QLabel(dialog);
        label->setObjectName("vBattLabel");
    }
    return dialog;
}

// The debug lines `recorder` saw, as text.
QStringList debugTexts(const fastecu::testing::SignalRecorder<void (MainWindow::*)(QString, bool, bool)>& recorder)
{
    QStringList texts;
    for (const auto& [message, timestamp, linefeed] : recorder.Snapshot())
    {
        texts << message;
    }
    return texts;
}

} // namespace

void MainWindowTest::checkLogWindowLinesReachEveryOpenOperationsWindow()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.vbatt_timer_->stop();
    std::vector<QDialog *> dialogs;
    std::ranges::transform(kOperationsWindowNames, std::back_inserter(dialogs),
                           [&window](const char *name) { return openOperationsWindow(window, name, false); });

    window.sendMessageToLogWindow("line\n");

    for (QDialog *dialog : dialogs)
    {
        SCOPED_TRACE(dialog->objectName().toStdString());
        EXPECT_EQ(dialog->findChild<QTextEdit *>("text_edit")->toPlainText(), "line\n");
    }
}

TEST_F(MainWindowTest, logWindowLinesReachEveryOpenOperationsWindow)
{
    ASSERT_NO_FATAL_FAILURE(checkLogWindowLinesReachEveryOpenOperationsWindow());
}

void MainWindowTest::checkBatteryReadingReachesEveryOperationsWindowWithALabel()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.vbatt_timer_->stop();
    // Only the ECU and DTC windows carry a battery label.
    QDialog *ecu = openOperationsWindow(window, "EcuOperationsWindow", true);
    openOperationsWindow(window, "BiuOperationsSubaruWindow", false);
    QDialog *dtc = openOperationsWindow(window, "DtcOperationsWindow", true);
    openOperationsWindow(window, "DataTerminalWindow", false);
    ON_CALL(*services.fake, GetUseOpenport2Adapter()).WillByDefault(::testing::Return(true));
    ON_CALL(*services.fake, ReadVbatt()).WillByDefault(::testing::Return(12345UL));
    fastecu::testing::SignalRecorder debugLines{&window, &MainWindow::logD};

    window.updateVbatt();

    EXPECT_EQ(ecu->findChild<QLabel *>("vBattLabel")->text(), "Battery: 12.345 V");
    EXPECT_EQ(dtc->findChild<QLabel *>("vBattLabel")->text(), "Battery: 12.345 V");
    EXPECT_EQ(debugTexts(debugLines).count("Battery: 12.345 V"), 2);
}

TEST_F(MainWindowTest, batteryReadingReachesEveryOperationsWindowWithALabel)
{
    ASSERT_NO_FATAL_FAILURE(checkBatteryReadingReachesEveryOperationsWindowWithALabel());
}

void MainWindowTest::checkOnlyTheOpenWindowsReceiveLogLines()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.vbatt_timer_->stop();
    QDialog *dtc = openOperationsWindow(window, "DtcOperationsWindow", false);

    window.sendMessageToLogWindow("only\n");

    EXPECT_EQ(dtc->findChild<QTextEdit *>("text_edit")->toPlainText(), "only\n");
}

TEST_F(MainWindowTest, onlyTheOpenWindowsReceiveLogLines)
{
    ASSERT_NO_FATAL_FAILURE(checkOnlyTheOpenWindowsReceiveLogLines());
}

void MainWindowTest::checkDataTerminalWindowAddsNoDebugLines()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.vbatt_timer_->stop();
    openOperationsWindow(window, "DataTerminalWindow", false);
    ON_CALL(*services.fake, GetUseOpenport2Adapter()).WillByDefault(::testing::Return(true));
    ON_CALL(*services.fake, ReadVbatt()).WillByDefault(::testing::Return(12345UL));
    fastecu::testing::SignalRecorder debugLines{&window, &MainWindow::logD};

    window.sendMessageToLogWindow("x");
    window.updateVbatt();

    EXPECT_TRUE(debugTexts(debugLines).filter(QRegularExpression("^Found dataTerminalWindow")).isEmpty())
        << qPrintable(debugTexts(debugLines).join(" | "));
}

TEST_F(MainWindowTest, dataTerminalWindowAddsNoDebugLines)
{
    ASSERT_NO_FATAL_FAILURE(checkDataTerminalWindowAddsNoDebugLines());
}

void MainWindowTest::checkWindowDestructionJoinsIdentificationWithoutContinuingLogging()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    auto window = std::make_unique<MainWindow>(services.services());
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(*window, *services.fake, "Subaru", "K-Line"));
    bool continued = false;
    window->connectToEcu([&continued](bool) { continued = true; });
    ASSERT_TRUE(window->connection_coordinator_->identifying());
    window.reset();
    fastecu::testing::ProcessEventsFor(std::chrono::milliseconds(200));
    ASSERT_TRUE(!continued);
}

TEST_F(MainWindowTest, windowDestructionJoinsIdentificationWithoutContinuingLogging)
{
    ASSERT_NO_FATAL_FAILURE(checkWindowDestructionJoinsIdentificationWithoutContinuingLogging());
}

void MainWindowTest::checkConnectStopsAnActiveLoggingWorkerBeforeIdentification()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    services.logging_engine.RegisterProtocol("SSM",
                                             [](const fastecu::logging::LoggingRunSnapshot&)
                                             {
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->BlockPollUntilCancelled();
                                                 return protocol;
                                             });
    fastecu::logging::LoggerModel model;
    model.InstallDefinition({.parameters = {{.protocol = "SSM",
                                             .id = "rpm",
                                             .address = "10",
                                             .length = "1",
                                             .enabled = true,
                                             .conversions = {{"rpm", "x", "0", "0", "100", "1"}}}}});
    model.SetSelection({.protocol = "SSM", .lower_panel_ids = {"rpm"}});
    const auto snapshot = fastecu::logging::PrepareLoggingRun(model, fastecu::logging::LoggingProtocolId::kSsm, "SSM",
                                                              {.poll_timeout = std::chrono::milliseconds{50},
                                                               .car_silence_miss_threshold = 20,
                                                               .reconnect_attempt_threshold = 100,
                                                               .reconnect_retry_period = 20},
                                                              fastecu::logging::LoggingTarget::kEcu);
    ASSERT_THAT(snapshot, fastecu::testing::IsOk());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    window.logging_run_->setCsvEnabled(true, directory.path(), "run");
    ASSERT_THAT(window.logging_run_->start({{"SSM"}, *snapshot, directory.path(), "run"}), fastecu::testing::IsOk());
    ASSERT_THAT(window.logging_run_->view().active_snapshot, Optional(_));
    ASSERT_TRUE(window.logging_run_->view().csv_open);
    window.logging_state_ = true;
    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_TRUE(!services.logging_engine.IsRunning());
    ASSERT_TRUE(!window.logging_state_);
    EXPECT_FALSE(window.logging_run_->view().csv_open);
    EXPECT_THAT(window.logging_run_->view().active_snapshot, Eq(std::nullopt));
}

TEST_F(MainWindowTest, connectStopsAnActiveLoggingWorkerBeforeIdentification)
{
    ASSERT_NO_FATAL_FAILURE(checkConnectStopsAnActiveLoggingWorkerBeforeIdentification());
}

struct ConnectionEntryPointsStopIdentificationCase
{
    std::string name;
    QString entry_point;
};
std::vector<ConnectionEntryPointsStopIdentificationCase> connectionEntryPointsStopIdentificationRows()
{
    constexpr std::array kNames{"logTransportChanged", "checkSerialPorts",    "openSerialPort",
                                "showDtcWindow",       "showSubaruBiuWindow", "showTerminalWindow"};
    return kNames |
           std::views::transform(
               [](const char *name)
               { return ConnectionEntryPointsStopIdentificationCase{name, QString::fromLatin1(name)}; }) |
           std::ranges::to<std::vector>();
}
class ConnectionEntryPointsStopIdentificationParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<ConnectionEntryPointsStopIdentificationCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, ConnectionEntryPointsStopIdentificationParameters,
                         ::testing::ValuesIn(connectionEntryPointsStopIdentificationRows()),
                         [](const ::testing::TestParamInfo<ConnectionEntryPointsStopIdentificationCase>& paramInfo)
                         {
                             auto name = paramInfo.param.name;
                             for (char& c : name)
                             {
                                 if (!std::isalnum(static_cast<unsigned char>(c)))
                                 {
                                     c = '_';
                                 }
                             }
                             return name;
                         });
void MainWindowTest::checkConnectionEntryPointsStopIdentification(QString entryPoint)
{

    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    bool cancelled = false;
    window.connectToEcu([&cancelled](bool connected) { cancelled = !connected; });
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_TRUE(!window.serial_port_list_->isEnabled());
    ASSERT_TRUE(!window.refresh_serial_port_list_->isEnabled());
    QTimer closeDialog;
    closeDialog.setInterval(5);
    QObject::connect(&closeDialog, &QTimer::timeout,
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
    closeDialog.start();
    if (entryPoint == "showDtcWindow")
    {
        window.showDtcWindow();
    }
    else if (entryPoint == "showSubaruBiuWindow")
    {
        window.showSubaruBiuWindow();
    }
    else if (entryPoint == "showTerminalWindow")
    {
        window.showTerminalWindow();
    }
    else
    {
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, entryPoint.toLatin1().constData(), Qt::DirectConnection));
    }
    ASSERT_TRUE(!window.connection_coordinator_->identifying());
    ASSERT_TRUE(cancelled);
    // A cancelled identification leaves no ECU connected, so the port
    // selector unlocks as it does after Disconnect.
    ASSERT_TRUE(window.serial_port_list_->isEnabled());
    ASSERT_TRUE(window.refresh_serial_port_list_->isEnabled());
    fastecu::testing::ProcessEventsFor(std::chrono::milliseconds(200));
    ASSERT_TRUE(!window.ecu_init_complete_);
}

TEST_P(ConnectionEntryPointsStopIdentificationParameters, connectionEntryPointsStopIdentification)
{
    ASSERT_NO_FATAL_FAILURE(checkConnectionEntryPointsStopIdentification(GetParam().entry_point));
}

void MainWindowTest::checkNestedConnectDuringCapabilityNoticeKeepsEachContinuation()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    EXPECT_CALL(*services.fake, ReadSerialData(::testing::_))
        .WillOnce(::testing::Return(kEcuInit))
        .WillRepeatedly(::testing::Return(QByteArray{}));
    std::optional<bool> firstResult;
    std::optional<bool> secondResult;
    window.connectToEcu([&firstResult](bool connected) { firstResult = connected; });
    ASSERT_TRUE(window.identify_launcher_->waitForWorker(std::chrono::milliseconds(5000)));
    bool restarted = false;
    bool targetFrozenInNotice = false;
    QTimer noticeDriver;
    noticeDriver.setInterval(5);
    QObject::connect(&noticeDriver, &QTimer::timeout,
                     [&]
                     {
                         for (QWidget *widget : QApplication::topLevelWidgets())
                         {
                             if (auto *notice = qobject_cast<QMessageBox *>(widget); notice && notice->isVisible())
                             {
                                 if (!restarted)
                                 {
                                     targetFrozenInNotice = !window.ecu_radio_button_->isEnabled() &&
                                                            !window.tcu_radio_button_->isEnabled();
                                     restarted = true;
                                     window.connectToEcu([&secondResult](bool connected) { secondResult = connected; });
                                 }
                                 notice->accept();
                             }
                         }
                     });
    noticeDriver.start();
    QCoreApplication::processEvents();
    noticeDriver.stop();
    ASSERT_TRUE(restarted);
    ASSERT_TRUE(targetFrozenInNotice);
    ASSERT_TRUE(!window.ecu_radio_button_->isEnabled());
    ASSERT_TRUE(!window.tcu_radio_button_->isEnabled());
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_THAT(firstResult, Optional(_));
    ASSERT_TRUE(!*firstResult);
    ASSERT_THAT(secondResult, Eq(std::nullopt));
    window.connection_coordinator_->cancel();
    ASSERT_THAT(secondResult, Optional(_));
    ASSERT_TRUE(!*secondResult);
    ASSERT_TRUE(window.ecu_radio_button_->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button_->isEnabled());
}

TEST_F(MainWindowTest, nestedConnectDuringCapabilityNoticeKeepsEachContinuation)
{
    ASSERT_NO_FATAL_FAILURE(checkNestedConnectDuringCapabilityNoticeKeepsEachContinuation());
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
void MainWindowTest::checkMenuMatchesTheGolden()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    const std::string actual = fastecu::ui::testing::menuSnapshot(*window.ui_->menubar, *window.ui_->toolBar);
    const char *goldenPath = std::getenv("MAIN_MENU_GOLDEN_PATH");
    ASSERT_NE(goldenPath, nullptr) << "MAIN_MENU_GOLDEN_PATH must be set by the Bazel target's env";
    std::ifstream file(goldenPath, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    const std::string expected((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(actual, expected) << "actual snapshot:\n" << actual;
}

TEST_F(MainWindowTest, menuMatchesTheGolden)
{
    ASSERT_NO_FATAL_FAILURE(checkMenuMatchesTheGolden());
}

void MainWindowTest::checkEveryIconNamedByTheMenuResolves()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    int withoutIcon = 0;
    for (const QAction *action : window.findChildren<QAction *>())
    {
        if (!action->objectName().startsWith(QStringLiteral("action")))
        {
            continue;
        }
        withoutIcon += action->icon().isNull() ? 1 : 0;
    }
    // Set value, Interpolate bidirectional, Log views, Hex Editor, Terminal,
    // BIU communication, Get Encryption Key, WinOLS CSV and ECU definition
    // manager have no icon by design; any other null icon is a mistyped path.
    EXPECT_EQ(withoutIcon, 9);
}

TEST_F(MainWindowTest, everyIconNamedByTheMenuResolves)
{
    ASSERT_NO_FATAL_FAILURE(checkEveryIconNamedByTheMenuResolves());
}

void MainWindowTest::checkNoTwoActionsShareAShortcutAndNoneLostItsBinding()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    QSet<QString> seen;
    for (const QAction *action : window.findChildren<QAction *>())
    {
        SCOPED_TRACE(::testing::PrintToString(action));
        for (const QKeySequence& sequence : action->shortcuts())
        {
            SCOPED_TRACE(::testing::PrintToString(sequence));
            const QString text = sequence.toString(QKeySequence::PortableText);
            EXPECT_FALSE(seen.contains(text)) << qPrintable(text) << " bound twice";
            seen.insert(text);
        }
    }
    for (const ActionName *name : {&kLogToFile, &kConnectToEcu, &kDisconnectFromEcu, &kToggleRealtime})
    {
        SCOPED_TRACE(::testing::PrintToString(name));
        EXPECT_FALSE(menuAction(window, *name)->shortcuts().isEmpty()) << name->member;
    }
    // Open, Save, Save As, Copy, Paste and Quit had shortcuts in menu.cfg. Qt
    // has no standard key for Save As or Quit on Windows, so each must still
    // be bound there through its fallback.
    for (const char *member : {"actionOpenCalibration", "actionSaveCalibration", "actionSaveCalibrationAs",
                               "actionCopy", "actionPaste", "actionQuit"})
    {
        SCOPED_TRACE(::testing::PrintToString(member));
        const auto *action = window.findChild<QAction *>(QString::fromLatin1(member));
        ASSERT_NE(action, nullptr) << member;
        EXPECT_FALSE(action->shortcut().isEmpty()) << member;
    }
}

TEST_F(MainWindowTest, noTwoActionsShareAShortcutAndNoneLostItsBinding)
{
    ASSERT_NO_FATAL_FAILURE(checkNoTwoActionsShareAShortcutAndNoneLostItsBinding());
}

void MainWindowTest::checkToolbarKeepsMenuActionsBeforeTheTransportWidgets()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    const QList<QAction *> actions = window.ui_->toolBar->actions();
    // Open, Save, |, Logging, Log to file, Read, Test write, Write, |, then widgets.
    ASSERT_GE(actions.size(), 10U);
    EXPECT_TRUE(actions[2]->isSeparator());
    EXPECT_TRUE(actions[8]->isSeparator());
    // widgetForAction cannot tell these apart (every action has a tool button,
    // separators have a separator widget); only a QWidgetAction hosts a widget
    // that MainWindow added in code.
    for (int i = 0; i < 9; ++i)
    {
        SCOPED_TRACE(::testing::PrintToString(i));
        EXPECT_EQ(qobject_cast<QWidgetAction *>(actions[i]), nullptr) << i;
    }
    EXPECT_NE(qobject_cast<QWidgetAction *>(actions[9]), nullptr);
}

TEST_F(MainWindowTest, toolbarKeepsMenuActionsBeforeTheTransportWidgets)
{
    ASSERT_NO_FATAL_FAILURE(checkToolbarKeepsMenuActionsBeforeTheTransportWidgets());
}

void MainWindowTest::checkAStaleOrMalformedMenuCfgIsIgnored()
{
    // A private root, so the garbage file never reaches the shared fixture.
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_NO_FATAL_FAILURE(copyFixtureConfig(root.path()));
    const QString menuCfg = root.path() + "/" + QString::fromStdString(kTestApplication.version) + "/config/menu.cfg";
    ASSERT_TRUE(writeTextFile(menuCfg, "<<< not xml >>>"));

    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{root.path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    // The runtime menu loader raised this warning for an unreadable menu.cfg;
    // ModalDriver accepts any box it does not recognise, so check its record.
    for (const QString& text : constructorDriver.acceptedTexts())
    {
        SCOPED_TRACE(::testing::PrintToString(text));
        EXPECT_FALSE(text.startsWith(QStringLiteral("Unable to load menu config file"))) << qPrintable(text);
    }
    EXPECT_NE(window.ui_->actionToggleRealtime, nullptr);
    EXPECT_EQ(window.ui_->menubar->findChildren<QMenu *>().size(), 7);
}

TEST_F(MainWindowTest, aStaleOrMalformedMenuCfgIsIgnored)
{
    ASSERT_NO_FATAL_FAILURE(checkAStaleOrMalformedMenuCfgIsIgnored());
}

void MainWindowTest::checkEveryMenuActionIsConnectedToTheWindow()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

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
    EXPECT_EQ(probed, 32);
    EXPECT_TRUE(unconnected.isEmpty()) << "not connected: " << qPrintable(unconnected.join(", "));
}

TEST_F(MainWindowTest, everyMenuActionIsConnectedToTheWindow)
{
    ASSERT_NO_FATAL_FAILURE(checkEveryMenuActionIsConnectedToTheWindow());
}

void MainWindowTest::checkTriggeringLogToFileReachesItsHandler()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();

    QAction *logToFile = menuAction(window, kLogToFile);
    ASSERT_NE(logToFile, nullptr);
    ASSERT_FALSE(logToFile->isChecked());
    ASSERT_FALSE(window.logging_run_->view().csv_enabled);

    // trigger() toggles the checkable action as a click does; the handler
    // then reads the new state.
    logToFile->trigger();
    EXPECT_TRUE(window.logging_run_->view().csv_enabled);
    logToFile->trigger();
    EXPECT_FALSE(window.logging_run_->view().csv_enabled);
}

TEST_F(MainWindowTest, triggeringLogToFileReachesItsHandler)
{
    ASSERT_NO_FATAL_FAILURE(checkTriggeringLogToFileReachesItsHandler());
}

void MainWindowTest::checkTuneActionsEditTheSelectionThroughTheirOwnHandlers()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
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
    services.config.Settings().primary_definition_base = "ecuflash";
    services.config.Settings().use_ecuflash_definitions = "enabled";
    services.config.Settings().ecuflash_definition_files_directory = files.path().toStdString();
    ASSERT_THAT(services.definition_catalogs.RefreshIndex(fastecu::definition::DefinitionFormat::kEcuFlash),
                fastecu::testing::IsOk());

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
    const auto opened = services.calibrations.AdoptReadImage({
        .rom = image,
        .filename = "grid.bin",
        .rom_id = "GRID",
    });
    ASSERT_THAT(opened, fastecu::testing::IsOk());
    ASSERT_TRUE(window.addCalibration(opened->id));

    auto *fileTree = window.ui_->calibrationFilesTreeWidget;
    ASSERT_EQ(fileTree->topLevelItemCount(), 1);
    fileTree->topLevelItem(0)->setSelected(true);
    window.calibrationFilesTreewidgetItemSelected(fileTree->topLevelItem(0));
    QTreeWidget *dataTree = window.ui_->calibrationDataTreeWidget;
    QTreeWidgetItem *gridItem = nullptr;
    for (int i = 0; i < dataTree->topLevelItemCount(); ++i)
    {
        if (dataTree->topLevelItem(i)->text(0) == "Tune" && dataTree->topLevelItem(i)->childCount() != 0)
        {
            gridItem = dataTree->topLevelItem(i)->child(0);
        }
    }
    ASSERT_NE(gridItem, nullptr);
    dataTree->setCurrentItem(gridItem);
    window.calibrationDataTreewidgetItemSelected(gridItem);
    const QList<QMdiSubWindow *> windows = window.ui_->mdiArea->subWindowList();
    ASSERT_EQ(windows.size(), 1U);
    window.ui_->mdiArea->setActiveSubWindow(windows.front());
    auto *table = windows.front()->findChild<QTableWidget *>(windows.front()->objectName());
    ASSERT_NE(table, nullptr);

    // Row 0 and column 0 of a 3D map's table are its axes; the body starts
    // at (1, 1).
    const auto select = [table](int top, int left, int bottom, int right)
    {
        table->clearSelection();
        table->setRangeSelected(QTableWidgetSelectionRange(top, left, bottom, right), true);
    };
    const auto grid = [&, id = opened->id]
    {
        const bytes::ByteView rom = services.calibrations.Find(id)->File();
        const auto first = rom.begin() + static_cast<std::ptrdiff_t>(kBody);
        return std::vector<int>(first, first + static_cast<std::ptrdiff_t>(body.size()));
    };

    // Chained on the centre cell (100): each step's size and sign tells the
    // four increment actions apart.
    const std::array<std::pair<QAction *, int>, 4> steps{{
        {window.ui_->actionCoarseIncrement, 110},
        {window.ui_->actionFineIncrement, 111},
        {window.ui_->actionFineDecrement, 110},
        {window.ui_->actionCoarseDecrement, 100},
    }};
    for (const auto& [action, expected] : steps)
    {
        SCOPED_TRACE(::testing::PrintToString(action));
        select(2, 2, 2, 2);
        action->trigger();
        EXPECT_EQ(grid()[4], expected) << qPrintable(action->objectName());
    }

    // Each interpolation over the whole body, from the same starting grid.
    const std::array<std::pair<QAction *, std::vector<int>>, 3> interpolations{{
        {window.ui_->actionInterpolateHorizontal, {0, 10, 20, 0, 0, 0, 40, 50, 60}},
        {window.ui_->actionInterpolateVertical, {0, 0, 20, 20, 0, 40, 40, 0, 60}},
        {window.ui_->actionInterpolateBidirectional, {0, 10, 20, 20, 30, 40, 40, 50, 60}},
    }};
    for (const auto& [action, expected] : interpolations)
    {
        SCOPED_TRACE(::testing::PrintToString(action));
        ASSERT_THAT(services.calibrations.Find(opened->id)->WriteBytes(kBody, body), fastecu::testing::IsOk());
        select(1, 1, 3, 3);
        action->trigger();
        EXPECT_EQ(grid(), expected) << qPrintable(action->objectName());
    }

    // Set Value edits the original window's selection as it stands when the
    // dialog is accepted, not when the dialog opened.
    ASSERT_THAT(services.calibrations.Find(opened->id)->WriteBytes(kBody, body), fastecu::testing::IsOk());
    select(2, 2, 2, 2);
    QTimer answer;
    answer.setInterval(5);
    QObject::connect(&answer, &QTimer::timeout,
                     [&]
                     {
                         auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                         if (dialog == nullptr)
                         {
                             return;
                         }
                         select(1, 1, 1, 1);
                         dialog->setTextValue("77");
                         dialog->accept();
                     });
    answer.start();
    window.ui_->actionSetValue->trigger();
    answer.stop();
    EXPECT_EQ(grid(), (std::vector<int>{77, 0, 20, 0, 100, 0, 40, 0, 60}));

    // Paste hands the full source layout to the backend, which starts at the
    // selection's top-left body cell and clips to the body's edges.
    ASSERT_THAT(services.calibrations.Find(opened->id)->WriteBytes(kBody, body), fastecu::testing::IsOk());
    QApplication::clipboard()->setText("1\t2\t3\n4\t5\t6\n7\t8\t9\n");
    select(2, 2, 2, 2);
    window.ui_->actionPaste->trigger();
    EXPECT_EQ(grid(), (std::vector<int>{0, 0, 20, 0, 1, 2, 40, 4, 5}));

    driver.stop();
    EXPECT_TRUE(driver.acceptedTexts().isEmpty()) << qPrintable(driver.acceptedTexts().join(" | "));
    ASSERT_TRUE(!driver.timedOut());
}

TEST_F(MainWindowTest, tuneActionsEditTheSelectionThroughTheirOwnHandlers)
{
    ASSERT_NO_FATAL_FAILURE(checkTuneActionsEditTheSelectionThroughTheirOwnHandlers());
}

void MainWindowTest::checkCopyFromALargerMapPastesIntoASmallerOneThroughItsScaling()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    constructorDriver.stop();
    ModalDriver driver{QString()};
    driver.start();
    window.show();
    QApplication::processEvents();

    // A 3x3 source scaled by 1/8 at 0x10 and a 2x2 destination scaled by 1/4
    // at 0x30. The source displays whole numbers, so only full-precision Copy
    // can carry its 2.5 into the destination as raw 10.
    QTemporaryDir files;
    ASSERT_TRUE(files.isValid());
    ASSERT_TRUE(writeTextFile(files.path() + "/maps.xml", R"(
<rom><romid><xmlid>COPY</xmlid></romid>
<scaling name="eighth" toexpr="x/8" frexpr="x*8" format="%.0f" min="0" max="40" inc="1" storagetype="uint8" endian="big"/>
<scaling name="quarter" toexpr="x/4" frexpr="x*4" format="%.2f" min="0" max="60" inc="1" storagetype="uint8" endian="big"/>
<table name="Source" category="Tune" address="10" type="3D" sizex="3" sizey="3" scaling="eighth" storagetype="uint8" endian="big">
<table type="X Axis" name="Column" address="0" elements="3" scaling="eighth" storagetype="uint8" endian="big"/>
<table type="Y Axis" name="Row" address="4" elements="3" scaling="eighth" storagetype="uint8" endian="big"/>
</table>
<table name="Dest" category="Tune" address="30" type="3D" sizex="2" sizey="2" scaling="quarter" storagetype="uint8" endian="big">
<table type="X Axis" name="Column" address="20" elements="2" scaling="quarter" storagetype="uint8" endian="big"/>
<table type="Y Axis" name="Row" address="24" elements="2" scaling="quarter" storagetype="uint8" endian="big"/>
</table></rom>)"));
    services.config.Settings().primary_definition_base = "ecuflash";
    services.config.Settings().use_ecuflash_definitions = "enabled";
    services.config.Settings().ecuflash_definition_files_directory = files.path().toStdString();
    ASSERT_THAT(services.definition_catalogs.RefreshIndex(fastecu::definition::DefinitionFormat::kEcuFlash),
                fastecu::testing::IsOk());

    constexpr std::size_t kSource = 0x10;
    constexpr std::size_t kDest = 0x30;
    const bytes::Bytes sourceBody{0, 20, 20, 0, 100, 0, 40, 0, 60};
    bytes::Bytes image(0x40, 0);
    // Distinct axis bytes: a copy that wrongly included them would show up.
    for (std::size_t i = 0; i < 3; ++i)
    {
        image[i] = static_cast<bytes::Byte>(201 + i);
        image[4 + i] = static_cast<bytes::Byte>(211 + i);
    }
    std::ranges::copy(sourceBody, image.begin() + static_cast<std::ptrdiff_t>(kSource));
    const auto opened = services.calibrations.AdoptReadImage({
        .rom = image,
        .filename = "maps.bin",
        .rom_id = "COPY",
    });
    ASSERT_THAT(opened, fastecu::testing::IsOk());
    ASSERT_TRUE(window.addCalibration(opened->id));

    auto *fileTree = window.ui_->calibrationFilesTreeWidget;
    ASSERT_EQ(fileTree->topLevelItemCount(), 1);
    fileTree->topLevelItem(0)->setSelected(true);
    window.calibrationFilesTreewidgetItemSelected(fileTree->topLevelItem(0));
    QTreeWidget *dataTree = window.ui_->calibrationDataTreeWidget;
    const auto openMap = [&](const QString& name) -> QMdiSubWindow *
    {
        for (int i = 0; i < dataTree->topLevelItemCount(); ++i)
        {
            auto *category = dataTree->topLevelItem(i);
            for (int child = 0; category->text(0) == "Tune" && child < category->childCount(); ++child)
            {
                if (category->child(child)->text(0) == name)
                {
                    dataTree->setCurrentItem(category->child(child));
                    window.calibrationDataTreewidgetItemSelected(category->child(child));
                    for (auto *candidate : window.ui_->mdiArea->subWindowList())
                    {
                        if (candidate->windowTitle().startsWith(name + " - "))
                        {
                            window.ui_->mdiArea->setActiveSubWindow(candidate);
                            return candidate;
                        }
                    }
                }
            }
        }
        return nullptr;
    };
    QMdiSubWindow *sourceWindow = openMap("Source");
    QMdiSubWindow *destWindow = openMap("Dest");
    ASSERT_NE(sourceWindow, nullptr);
    ASSERT_NE(destWindow, nullptr);
    auto *sourceTable = sourceWindow->findChild<QTableWidget *>();
    auto *destTable = destWindow->findChild<QTableWidget *>();
    ASSERT_NE(sourceTable, nullptr);
    ASSERT_NE(destTable, nullptr);

    const auto destBody = [&, id = opened->id]
    {
        const bytes::ByteView rom = services.calibrations.Find(id)->File();
        const auto first = rom.begin() + static_cast<std::ptrdiff_t>(kDest);
        return std::vector<int>(first, first + 4);
    };

    // Select All takes the body only, so the clipboard holds the 3x3 values
    // at full precision and none of the axes.
    window.ui_->mdiArea->setActiveSubWindow(sourceWindow);
    sourceTable->setFocus();
    QKeyEvent selectAll(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(sourceTable, &selectAll);
    window.ui_->actionCopy->trigger();
    EXPECT_EQ(QApplication::clipboard()->text(), "0\t2.5\t2.5\n0\t12.5\t0\n5\t0\t7.5");

    // Paste starts at the destination selection's top-left and clips to its
    // 2x2 body, storing each value through the destination's own scaling.
    window.ui_->mdiArea->setActiveSubWindow(destWindow);
    destTable->clearSelection();
    destTable->setRangeSelected(QTableWidgetSelectionRange(1, 1, 1, 1), true);
    window.ui_->actionPaste->trigger();
    EXPECT_EQ(destBody(), (std::vector<int>{0, 10, 0, 50}));

    driver.stop();
    EXPECT_TRUE(driver.acceptedTexts().isEmpty()) << qPrintable(driver.acceptedTexts().join(" | "));
    ASSERT_TRUE(!driver.timedOut());
}

TEST_F(MainWindowTest, copyFromALargerMapPastesIntoASmallerOneThroughItsScaling)
{
    ASSERT_NO_FATAL_FAILURE(checkCopyFromALargerMapPastesIntoASmallerOneThroughItsScaling());
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment);
class MainWindowFixtureEnvironment : public ::testing::Environment
{
  public:
    void TearDown() override
    {
        MainWindowTest::config_root_.reset();
    }
};
const auto *const kFixtureEnvironment = ::testing::AddGlobalTestEnvironment(new MainWindowFixtureEnvironment);
} // namespace

fastecu::definition::RomDefinition MainWindowTest::assignmentDefinition(const AssignmentCase& c)
{
    fastecu::definition::RomDefinition definition;
    definition.scalings.push_back({.name = "Raw",
                                   .from_byte = c.from_byte,
                                   .to_byte = c.to_byte,
                                   .format = "0.00",
                                   .storage_type = fastecu::definition::StorageType::kInt16,
                                   .endian = "big"});
    definition.scalings[0].fine_increment = c.fine_increment;
    definition.scalings[0].maximum = c.maximum;
    fastecu::definition::CalibrationMap model;
    model.name = "Value";
    model.category = "Controls";
    model.type = "1D";
    model.address = fastecu::memory::DefinitionAddress{0};
    model.storage_type = fastecu::definition::StorageType::kInt16;
    model.endian = "big";
    model.scaling_name = "Raw";
    definition.maps.push_back(model);
    return definition;
}

namespace
{
void replaceContents(TestServices& services, fastecu::calibration::SessionId id,
                     const fastecu::definition::RomDefinition& definition, std::vector<std::uint8_t> bytes)
{
    auto *session = services.calibrations.Find(id);
    ASSERT_NE(session, nullptr);
    *session = fastecu::calibration::CalibrationSession(
        id, {.source = session->Source(),
             .image = fastecu::memory::testing::IdentityImage(std::move(bytes)),
             .definition = fastecu::calibration::ResolvedDefinition{.definition = definition}});
}

// The map item under the last "Controls" category of the data tree.
QTreeWidgetItem *controlsMapItem(QTreeWidget *tree)
{
    QTreeWidgetItem *item = nullptr;
    for (int row = 0; row < tree->topLevelItemCount(); ++row)
    {
        if (tree->topLevelItem(row)->text(0) == "Controls")
        {
            item = tree->topLevelItem(row)->child(0);
        }
    }
    return item;
}
} // namespace

void MainWindowTest::checkTypedAssignment(const AssignmentCase& c)
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    QTemporaryDir files;
    ASSERT_TRUE(window.openCalibrationFile(writeRom(files, "edit.bin", '\x11')));
    driver.stop();
    const auto id = window.calibrations_.front().id;
    const auto definition = assignmentDefinition(c);
    ASSERT_NO_FATAL_FAILURE(replaceContents(services, id, definition, {0, 10}));
    window.calibrationFilesTreewidgetItemSelected(window.ui_->calibrationFilesTreeWidget->topLevelItem(0));
    QMdiSubWindow *subwindow = nullptr;
    QTableWidget *table = nullptr;
    ASSERT_NO_FATAL_FAILURE(openAssignmentMap(window, subwindow, table));
    std::optional<fastecu::calibration::SessionId> other;
    QMdiSubWindow *otherWindow = nullptr;
    if (c.other != AssignmentOther::kNone)
    {
        ASSERT_NO_FATAL_FAILURE(addOtherCalibration(window, services, definition, c, subwindow, other, otherWindow));
    }

    bool answered = false;
    QString noticeText;
    QTimer reply;
    reply.setInterval(5);
    QObject::connect(&reply, &QTimer::timeout,
                     [&] { answerAssignmentDialog(c, window, services, id, otherWindow, answered, noticeText); });
    reply.start();
    switch (c.action)
    {
    case AssignmentAction::kPaste:
        QApplication::clipboard()->setText(c.clipboard);
        answered = true;
        window.pasteValue();
        break;
    case AssignmentAction::kFineUp:
        window.incDecValue(fastecu::calibration::IncrementStep::kFineUp);
        break;
    case AssignmentAction::kSet:
        window.setValue();
        break;
    }
    reply.stop();
    ASSERT_TRUE(answered);
    verifyAssignment(c, services, id, other, table, noticeText);
}

// Opens the edited map's window, active, with its one cell selected.
void MainWindowTest::openAssignmentMap(MainWindow& window, QMdiSubWindow *& subwindow, QTableWidget *& table)
{
    auto *tree = window.ui_->calibrationDataTreeWidget;
    QTreeWidgetItem *item = controlsMapItem(tree);
    ASSERT_NE(item, nullptr);
    tree->setCurrentItem(item);
    window.calibrationDataTreewidgetItemSelected(item);
    ASSERT_EQ(window.ui_->mdiArea->subWindowList().size(), 1);
    subwindow = window.ui_->mdiArea->subWindowList().front();
    window.ui_->mdiArea->setActiveSubWindow(subwindow);
    table = subwindow->findChild<QTableWidget *>();
    ASSERT_NE(table, nullptr);
    table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 0, 0), true);
}

// Adds a second ROM with the same definition holding 40; with
// AssignmentOther::kWindow its map window opens too, behind the edited one.
void MainWindowTest::addOtherCalibration(MainWindow& window, TestServices& services,
                                         const fastecu::definition::RomDefinition& definition, const AssignmentCase& c,
                                         QMdiSubWindow *subwindow,
                                         std::optional<fastecu::calibration::SessionId>& other,
                                         QMdiSubWindow *& otherWindow)
{
    const auto adopted = services.calibrations.AdoptReadImage({.rom = {0, 40}, .filename = "other.bin"});
    ASSERT_THAT(adopted, fastecu::testing::IsOk());
    other = adopted->id;
    ASSERT_NO_FATAL_FAILURE(replaceContents(services, *other, definition, {0, 40}));
    ASSERT_TRUE(window.addCalibration(*other));
    if (c.other != AssignmentOther::kWindow)
    {
        return;
    }
    QTreeWidgetItem *item = controlsMapItem(window.ui_->calibrationDataTreeWidget);
    ASSERT_NE(item, nullptr);
    window.calibrationDataTreewidgetItemSelected(item);
    for (auto *candidate : window.ui_->mdiArea->subWindowList())
    {
        if (candidate != subwindow)
        {
            otherWindow = candidate;
        }
    }
    ASSERT_NE(otherWindow, nullptr);
    window.ui_->mdiArea->setActiveSubWindow(subwindow);
}

// Records and accepts a notice; otherwise applies the case's changes while
// the value dialog is open, then types its value.
void MainWindowTest::answerAssignmentDialog(const AssignmentCase& c, MainWindow& window, TestServices& services,
                                            fastecu::calibration::SessionId id, QMdiSubWindow *otherWindow,
                                            bool& answered, QString& noticeText)
{
    if (auto *notice = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()); notice != nullptr)
    {
        noticeText = notice->text();
        answered = true;
        notice->accept();
        return;
    }
    auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
    if (dialog == nullptr)
    {
        if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()); modal != nullptr)
        {
            modal->reject();
        }
        return;
    }
    if (c.write_during_dialog)
    {
        EXPECT_THAT(services.calibrations.Find(id)->WriteBytes(0, bytes::Bytes{0, 20}), fastecu::testing::IsOk());
    }
    if (c.select_other_during_dialog)
    {
        auto *fileTree = window.ui_->calibrationFilesTreeWidget;
        for (int row = 0; row < fileTree->topLevelItemCount(); ++row)
        {
            fileTree->topLevelItem(row)->setSelected(row == fileTree->topLevelItemCount() - 1);
        }
        window.calibrationFilesTreewidgetItemSelected(fileTree->topLevelItem(fileTree->topLevelItemCount() - 1));
    }
    if (otherWindow != nullptr)
    {
        window.ui_->mdiArea->setActiveSubWindow(otherWindow);
    }
    if (c.close_during_dialog)
    {
        window.closeCalibration();
    }
    dialog->setTextValue(c.typed);
    answered = true;
    dialog->accept();
}

void MainWindowTest::verifyAssignment(const AssignmentCase& c, TestServices& services,
                                      fastecu::calibration::SessionId id,
                                      std::optional<fastecu::calibration::SessionId> other, QTableWidget *table,
                                      const QString& noticeText)
{
    const auto *session = services.calibrations.Find(id);
    if (!c.value.has_value())
    {
        EXPECT_EQ(session, nullptr);
        return;
    }
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(bytes::ReadU16Be(session->File()), *c.value);
    if (c.dirty.has_value())
    {
        EXPECT_EQ(session->Dirty(), *c.dirty);
    }
    if (!c.cell.isEmpty())
    {
        EXPECT_EQ(table->item(0, 0)->text(), c.cell);
    }
    if (c.notice.has_value())
    {
        EXPECT_EQ(!noticeText.isEmpty(), *c.notice);
    }
    if (!c.notice_contains.isEmpty())
    {
        EXPECT_TRUE(noticeText.contains(c.notice_contains));
    }
    if (!c.notice_excludes.isEmpty())
    {
        EXPECT_FALSE(noticeText.contains(c.notice_excludes));
    }
    if (other.has_value())
    {
        EXPECT_EQ(bytes::ReadU16Be(services.calibrations.Find(*other)->File()), 40);
        EXPECT_FALSE(services.calibrations.Find(*other)->Dirty());
    }
}

TEST_F(MainWindowTest, SignedLiteralAssignsAbsoluteValue)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment({.typed = "-20", .value = 65516}));
}
TEST_F(MainWindowTest, VariableExpressionEditsRelativeValue)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment({.typed = "x-20", .value = 65526}));
}
TEST_F(MainWindowTest, AssignmentUsesBytesChangedDuringDialog)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment({.write_during_dialog = true, .value = 21}));
}
TEST_F(MainWindowTest, AssignmentKeepsOriginalSessionDuringSelectionChange)
{
    ASSERT_NO_FATAL_FAILURE(
        checkTypedAssignment({.other = AssignmentOther::kSession, .select_other_during_dialog = true, .value = 11}));
}
TEST_F(MainWindowTest, AssignmentOfClosedOriginalSessionIsInert)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment({.close_during_dialog = true}));
}
TEST_F(MainWindowTest, AssignmentCanLeaveBrokenDecodeDisplayedAsNan)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment({.from_byte = "1/0", .typed = "20", .value = 20, .cell = "NaN"}));
}

TEST_F(MainWindowTest, SubResolutionIncrementReportsNoChangeAndKeepsClean)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment({.from_byte = "x/10",
                                                  .to_byte = "x*10",
                                                  .fine_increment = "0.01",
                                                  .action = AssignmentAction::kFineUp,
                                                  .value = 10,
                                                  .dirty = false,
                                                  .notice_contains = "storage resolution"}));
}
TEST_F(MainWindowTest, ClampedIncrementReportsLimitAndKeepsClean)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment({.from_byte = "x/10",
                                                  .to_byte = "x*10",
                                                  .fine_increment = "0.01",
                                                  .maximum = "1",
                                                  .action = AssignmentAction::kFineUp,
                                                  .value = 10,
                                                  .dirty = false,
                                                  .notice_contains = "definition limit",
                                                  .notice_excludes = "storage resolution"}));
}

TEST_F(MainWindowTest, NoOpAssignmentRefreshesBytesChangedDuringDialog)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment(
        {.write_during_dialog = true, .typed = "20", .value = 20, .cell = "20.00", .dirty = true}));
}

TEST_F(MainWindowTest, AssignmentRefreshesOriginalAfterActiveMapChanges)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment({.other = AssignmentOther::kWindow, .value = 11, .cell = "11.00"}));
}
TEST_F(MainWindowTest, NoOpAssignmentRefreshesOriginalAfterActiveMapChanges)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment({.other = AssignmentOther::kWindow,
                                                  .write_during_dialog = true,
                                                  .typed = "20",
                                                  .value = 20,
                                                  .cell = "20.00",
                                                  .dirty = true}));
}
TEST_F(MainWindowTest, PasteAcceptsTerminalLf)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment(
        {.action = AssignmentAction::kPaste, .clipboard = "20\n", .value = 20, .cell = "20.00", .notice = false}));
}
TEST_F(MainWindowTest, PasteAcceptsTerminalCrLf)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment(
        {.action = AssignmentAction::kPaste, .clipboard = "20\r\n", .value = 20, .cell = "20.00", .notice = false}));
}
TEST_F(MainWindowTest, PasteRejectsInteriorEmptyCellAtomically)
{
    ASSERT_NO_FATAL_FAILURE(checkTypedAssignment(
        {.action = AssignmentAction::kPaste, .clipboard = "20\n\n30", .value = 10, .dirty = false, .notice = true}));
}

void MainWindowTest::checkTargetChangeInvalidatesIdentificationAndSupport()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    MainWindow window{services.services()};
    prepareLogging(window, "SSM");
    for (const auto oldSupport : {fastecu::logging::EcuSupport::kSupported, fastecu::logging::EcuSupport::kUnsupported})
    {
        SCOPED_TRACE(::testing::PrintToString(oldSupport));
        window.ecu_init_complete_ = true;
        window.ecuid_ = "OLD_TARGET";
        window.logger_model_->SetParameterSupport("SSM", "rpm", oldSupport);
        (window.ecu_radio_button_->isChecked() ? window.tcu_radio_button_ : window.ecu_radio_button_)->click();
        EXPECT_FALSE(window.ecu_init_complete_);
        EXPECT_TRUE(window.ecuid_.isEmpty());
        EXPECT_EQ(window.logger_model_->ParameterSupport("SSM", "rpm"), fastecu::logging::EcuSupport::kUnknown);
    }
    driver.stop();
}

TEST_F(MainWindowTest, targetChangeInvalidatesIdentificationAndSupport)
{
    ASSERT_NO_FATAL_FAILURE(checkTargetChangeInvalidatesIdentificationAndSupport());
}

TEST_F(MainWindowTest, replacementDuringSampleNotificationDiscardsOldBatch)
{
    ASSERT_NO_FATAL_FAILURE(checkReplacementDuringSampleNotificationDiscardsOldBatch());
}

TEST_F(MainWindowTest, queuedStopDuringRenderingPreventsTheCsvRow)
{
    ASSERT_NO_FATAL_FAILURE(checkQueuedStopDuringRenderingPreventsTheCsvRow());
}

void MainWindowTest::checkWindowDestructionStopsLoggingWithoutAModalNotice()
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    auto window = std::make_unique<MainWindow>(services.services());
    driver.stop();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    prepareLogging(*window, "SSM");
    window->config_session_->Settings().selected_log_protocol = "SSM";
    services.logging_engine.RegisterProtocol("SSM",
                                             [](const auto&)
                                             {
                                                 auto source = std::make_unique<ScriptedLoggingProtocol>();
                                                 source->BlockPollUntilCancelled();
                                                 return source;
                                             });
    window->config_session_->Settings().datalog_files_directory = directory.path().toStdString();
    window->logging_run_->setCsvEnabled(true, directory.path(), "run");
    window->continueStartLogging();
    ASSERT_TRUE(window->logging_run_->view().csv_open);
    bool modalNotice = false;
    const auto subscription =
        QObject::connect(qApp, &QApplication::focusChanged, qApp,
                         [&](QWidget *, QWidget *current)
                         {
                             if (current != nullptr && qobject_cast<QMessageBox *>(current->window()) != nullptr)
                             {
                                 modalNotice = true;
                             }
                         });
    window.reset();
    QObject::disconnect(subscription);
    EXPECT_FALSE(services.logging_engine.IsRunning());
    EXPECT_FALSE(modalNotice);
}
TEST_F(MainWindowTest, windowDestructionStopsLoggingWithoutAModalNotice)
{
    ASSERT_NO_FATAL_FAILURE(checkWindowDestructionStopsLoggingWithoutAModalNotice());
}
