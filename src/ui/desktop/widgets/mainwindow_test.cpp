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
#include "src/backend/config/catalog.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"
#include "src/ui/desktop/config_fields.h"
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
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *messageBox = qobject_cast<QMessageBox *>(widget); messageBox != nullptr)
            {
                if (messageBox->text() == kTcuChooserText)
                {
                    saw_chooser_ = true;
                    if (chooser_choice_.isEmpty())
                    {
                        messageBox->reject();
                        return;
                    }
                    for (QAbstractButton *button : messageBox->buttons())
                    {
                        if (button->text() == chooser_choice_)
                        {
                            button->click();
                            return;
                        }
                    }
                }
                if (messageBox->text() == kTcuIgnitionText)
                {
                    ++ignition_count_;
                    messageBox->done(QMessageBox::Cancel);
                    return;
                }
                if (messageBox->text() == kLegacyEcuIgnitionText)
                {
                    ++legacy_ecu_ignition_count_;
                    messageBox->done(QMessageBox::Cancel);
                    return;
                }
                if (messageBox->text().startsWith(kNoChecksumModuleText))
                {
                    ++checksum_warning_count_;
                    messageBox->done(QMessageBox::Cancel);
                    return;
                }
                if (messageBox->text() == kPortableEcuIgnitionText)
                {
                    ++portable_ecu_ignition_count_;
                    messageBox->done(QMessageBox::Cancel);
                    return;
                }
                if (messageBox->text() == kNoFileSelectedText)
                {
                    ++no_file_selected_count_;
                }

                accepted_texts_ << messageBox->text();
                messageBox->accept();
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
            if (dialog->objectName() == "BiuOperationsSubaruWindow")
            {
                dialog->reject(); // the BIU window has no work to do here
                return;
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
int startEcuOperations(MainWindow& window, const QString& cmdType)
{
    int result = -1;
    if (!QMetaObject::invokeMethod(&window, "start_ecu_operations", Qt::DirectConnection, Q_RETURN_ARG(int, result),
                                   Q_ARG(QString, cmdType)))
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
            .logger_definitions = logger_definitions,
            .config_repository = file_repository,
            .file_action_events = events,
            .log = log_channel,
            .connection = adapter.connection(),
            .remote = remote_peer,
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
    fastecu::logging::LoggerDefinitionService logger_definitions{file_repository, resource_bundle, file_writer};
    fastecu::ui::LogChannel log_channel;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.fake(); // null if the fake backend failed to start
    fastecu::ui::RemotePeer remote_peer;
    fastecu::desktop::logging::LoggingEngine logging_engine;
    std::function<std::unique_ptr<fastecu::IClock>()> make_clock{[]() -> std::unique_ptr<fastecu::IClock>
                                                                 { return std::make_unique<QtClock>(); }};
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
        ASSERT_TRUE(window.config_session_->SelectRow(static_cast<std::size_t>(it - vehicles.begin())).has_value());
    }

    // Selects the fixture's first vehicle row of `make`.
    static void selectMake(MainWindow& window, const QString& make)
    {
        const auto vehicles = window.config_session_->Vehicles();
        const auto it = std::ranges::find(vehicles, make.toStdString(), &VehicleSpec::make);
        ASSERT_TRUE(it != vehicles.end());
        ASSERT_TRUE(window.config_session_->SelectRow(static_cast<std::size_t>(it - vehicles.begin())).has_value());
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
        ON_CALL(fake, open_serial_port()).WillByDefault(::testing::Return(QString("ttyUSB0")));
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
        window.logger_values_.initialize(*window.logger_model_);
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
        window.save_calibration_file_as();
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
    void check_explicitConfigRootLoadsFixtureAndProvisionsDirectories();
    void check_directSessionStartupNeverWaitsForARemoteSource();
    void check_windowLogLinesReachTheLogChannel();
    void check_windowEnablesFileLoggingThroughTheChannel();
    void check_directSessionStartupNeverRequestsTheRemoteWait();
    void check_externalLoggerMirrorsToTheRemotePeer();
    void check_peerStateChangesReachTheWindow();
    void check_handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling(QString choice,
                                                                                   int expectedIgnitionCount);
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
    void check_failedMapDecodeKeepsAnErrorView();
    enum class AssignmentScenario
    {
        kAbsolute,
        kRelative,
        kCurrentBytes,
        kCurrentBytesNoOp,
        kSelectionChanged,
        kActiveMapChanged,
        kActiveMapChangedNoOp,
        kPasteLf,
        kPasteCrLf,
        kPasteInteriorEmpty,
        kOriginalClosed,
        kInvalidCurrent,
        kNoOpResolution,
        kNoOpLimit
    };
    void check_typedAssignment(AssignmentScenario scenario);
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
    void check_flashActionsFollowTheSelectedProtocolsCapabilities();
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
    void check_loggingStartWaitsForIdentification(bool targetIsEcu);
    void check_batterySamplingDoesNotUseTheFacadeDuringIdentification();
    void check_windowDestructionJoinsIdentificationWithoutContinuingLogging();
    void check_connectStopsAnActiveLoggingWorkerBeforeIdentification();
    void check_connectionEntryPointsStopIdentification(QString entryPoint);
    void check_nestedConnectDuringCapabilityNoticeKeepsEachContinuation();
    void check_menuMatchesTheGolden();
    void check_everyIconNamedByTheMenuResolves();
    void check_noTwoActionsShareAShortcutAndNoneLostItsBinding();
    void check_toolbarKeepsMenuActionsBeforeTheTransportWidgets();
    void check_aStaleOrMalformedMenuCfgIsIgnored();
    void check_everyMenuActionIsConnectedToTheWindow();
    void check_triggeringLogToFileReachesItsHandler();
    void check_tuneActionsEditTheSelectionThroughTheirOwnHandlers();
    void check_copyFromALargerMapPastesIntoASmallerOneThroughItsScaling();
};

void MainWindowTest::SetUpTestSuite()
{
    ASSERT_TRUE(fastecu::config::CatalogProblems(kWindowCatalog).empty());
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

void MainWindowTest::check_explicitConfigRootLoadsFixtureAndProvisionsDirectories()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();

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
    ASSERT_NO_FATAL_FAILURE(check_explicitConfigRootLoadsFixtureAndProvisionsDirectories());
}

void MainWindowTest::check_directSessionStartupNeverWaitsForARemoteSource()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    EXPECT_CALL(*services.fake, waitForSource()).Times(0);
    MainWindow window{services.services()};
    constructorDriver.stop();
}

TEST_F(MainWindowTest, directSessionStartupNeverWaitsForARemoteSource)
{
    ASSERT_NO_FATAL_FAILURE(check_directSessionStartupNeverWaitsForARemoteSource());
}

void MainWindowTest::check_windowLogLinesReachTheLogChannel()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
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
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    fastecu::testing::SignalRecorder enables{&services.log_channel, &fastecu::ui::LogChannel::enable_log_write_to_file};
    MainWindow window{services.services()};
    constructorDriver.stop();

    ASSERT_TRUE(std::ranges::any_of(enables.snapshot(), [](const auto& arguments) { return std::get<0>(arguments); }));
}

TEST_F(MainWindowTest, windowEnablesFileLoggingThroughTheChannel)
{
    ASSERT_NO_FATAL_FAILURE(check_windowEnablesFileLoggingThroughTheChannel());
}

void MainWindowTest::check_directSessionStartupNeverRequestsTheRemoteWait()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    fastecu::testing::SignalRecorder waits{&services.remote_peer, &fastecu::ui::RemotePeer::wait_requested};
    MainWindow window{services.services()};
    constructorDriver.stop();

    ASSERT_EQ(waits.count(), 0U);
}

TEST_F(MainWindowTest, directSessionStartupNeverRequestsTheRemoteWait)
{
    ASSERT_NO_FATAL_FAILURE(check_directSessionStartupNeverRequestsTheRemoteWait());
}

void MainWindowTest::check_externalLoggerMirrorsToTheRemotePeer()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
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
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    fastecu::testing::SignalRecorder debugLines{&window, &MainWindow::LOG_D};

    emit services.remote_peer.stateChanged(QRemoteObjectReplica::Valid, QRemoteObjectReplica::Default);

    ASSERT_TRUE(std::ranges::any_of(debugLines.snapshot(), [](const auto& arguments)
                                    { return std::get<0>(arguments) == "Network connection established"; }));
}

TEST_F(MainWindowTest, peerStateChangesReachTheWindow)
{
    ASSERT_NO_FATAL_FAILURE(check_peerStateChangesReachTheWindow());
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
INSTANTIATE_TEST_SUITE_P(
    Rows, HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingParameters,
    ::testing::ValuesIn(handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingRows()),
    [](const ::testing::TestParamInfo<HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingCase>& info)
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
    QString choice, int expectedIgnitionCount)
{

    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
    // Characterization: the call counts observed on master (before step
    // 6d) for both rows, pinned so the dispatch refactor cannot change them.
    EXPECT_CALL(*fake, reset_connection()).Times(3);
    EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(2);

    // A short poll period keeps the "timer stays stopped" check below quick.
    window.vbatt_timer_->setInterval(10);
    window.serial_ports_ = {"OpenPort 2.0"};
    window.serial_port_list_->clear();
    window.serial_port_list_->addItem("OpenPort 2.0");
    window.serial_port_list_->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_tcu_denso_sh7058_can"));

    // The TCU log lines are relayed through MainWindow's own LOG_* signals.
    fastecu::testing::SignalRecorder infoLines{&window, &MainWindow::LOG_I};
    ModalDriver operationDriver{choice};
    operationDriver.start();
    ASSERT_EQ(startEcuOperations(window, "read"), 0);

    ASSERT_TRUE(operationDriver.sawChooser());
    ASSERT_EQ(operationDriver.ignitionCount(), expectedIgnitionCount);
    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.unexpectedFlashDialogCount(), 0);

    fastecu::testing::process_events_for(std::chrono::milliseconds(window.vbatt_timer_->interval() + 100));
    ASSERT_TRUE(!window.vbatt_timer_->isActive());
    ASSERT_TRUE(window.calibrations_.empty());
    ASSERT_TRUE(services.calibrations.Ids().empty());
    const QString expectedLine = choice.isEmpty() ? "No option selected" : "Attempting TCU relearn";
    ASSERT_TRUE(std::ranges::any_of(infoLines.snapshot(),
                                    [&](const auto& arguments) { return std::get<0>(arguments) == expectedLine; }));
}

TEST_P(HandledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingParameters,
       handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling)
{
    ASSERT_NO_FATAL_FAILURE(check_handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling(
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
    [](const ::testing::TestParamInfo<FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoCase>& info)
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

    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
    window.serial_ports_ = {"OpenPort 2.0"};
    window.serial_port_list_->clear();
    window.serial_port_list_->addItem("OpenPort 2.0");
    window.serial_port_list_->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, protocol));

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_EQ(startEcuOperations(window, "read"), 0);
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.legacyEcuIgnitionCount(), 0);
    ASSERT_EQ(operationDriver.portableEcuIgnitionCount(), 0);
    ASSERT_EQ(operationDriver.unexpectedFlashDialogCount(), 0);
}

TEST_P(FutureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoParameters,
       futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo)
{
    ASSERT_NO_FATAL_FAILURE(check_futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo(GetParam().protocol));
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
    [](const ::testing::TestParamInfo<RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackCase>& info)
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

    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
    window.serial_ports_ = {"OpenPort 2.0"};
    window.serial_port_list_->clear();
    window.serial_port_list_->addItem("OpenPort 2.0");
    window.serial_port_list_->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, protocol));

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_EQ(startEcuOperations(window, "read"), 0);
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.legacyEcuIgnitionCount(), 0);
    ASSERT_EQ(operationDriver.portableEcuIgnitionCount(), 1);
}

TEST_P(RepresentativePortableRoutesReachFactoryBeforeLegacyFallbackParameters,
       representativePortableRoutesReachFactoryBeforeLegacyFallback)
{
    ASSERT_NO_FATAL_FAILURE(check_representativePortableRoutesReachFactoryBeforeLegacyFallback(GetParam().protocol));
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
void MainWindowTest::check_writeWithoutASelectedCalibrationStopsVoltagePolling(QString command)
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
    window.serial_ports_ = {"OpenPort 2.0"};
    window.serial_port_list_->clear();
    window.serial_port_list_->addItem("OpenPort 2.0");
    window.serial_port_list_->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can"));

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_EQ(startEcuOperations(window, command), 0);
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.noFileSelectedCount(), 1);
    ASSERT_TRUE(!window.vbatt_timer_->isActive());
}

TEST_P(WriteWithoutASelectedCalibrationStopsVoltagePollingParameters,
       writeWithoutASelectedCalibrationStopsVoltagePolling)
{
    ASSERT_NO_FATAL_FAILURE(check_writeWithoutASelectedCalibrationStopsVoltagePolling(GetParam().command));
}

void MainWindowTest::check_otherMakesSkipDispatchButStillRunCleanup()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
    window.serial_ports_ = {"OpenPort 2.0"};
    window.serial_port_list_->clear();
    window.serial_port_list_->addItem("OpenPort 2.0");
    window.serial_port_list_->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectMake(window, "Nissan"));

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_EQ(startEcuOperations(window, "read"), 0);
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.unexpectedFlashDialogCount(), 0);
    ASSERT_TRUE(!window.vbatt_timer_->isActive());
}

TEST_F(MainWindowTest, otherMakesSkipDispatchButStillRunCleanup)
{
    ASSERT_NO_FATAL_FAILURE(check_otherMakesSkipDispatchButStillRunCleanup());
}

void MainWindowTest::check_readOfAnUnsupportedProtocolAddsNoCalibration()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();

    window.serial_ports_ = {"OpenPort 2.0"};
    window.serial_port_list_->clear();
    window.serial_port_list_->addItem("OpenPort 2.0");
    window.serial_port_list_->setCurrentIndex(0);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_not_a_real_protocol"));
    ASSERT_EQ(window.calibrations_.size(), std::size_t{0});

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_EQ(startEcuOperations(window, "read"), 0);
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(window.calibrations_.size(), std::size_t{0});
    ASSERT_TRUE(services.calibrations.Ids().empty());
    ASSERT_EQ(window.ui_->calibrationFilesTreeWidget->topLevelItemCount(), 0);
}

TEST_F(MainWindowTest, readOfAnUnsupportedProtocolAddsNoCalibration)
{
    ASSERT_NO_FATAL_FAILURE(check_readOfAnUnsupportedProtocolAddsNoCalibration());
}

class CancellingTheChecksumWarningStopsVoltagePollingParameters : public MainWindowTest,
                                                                  public ::testing::WithParamInterface<WriteCommandCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, CancellingTheChecksumWarningStopsVoltagePollingParameters,
                         ::testing::ValuesIn(writeCommandRows()), writeCommandName);
void MainWindowTest::check_cancellingTheChecksumWarningStopsVoltagePolling(QString command)
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();

    FakeBackend *fake = services.fake;
    EXPECT_CALL(*fake, open_serial_port()).Times(0);
    EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
    window.serial_ports_ = {"OpenPort 2.0"};
    window.serial_port_list_->clear();
    window.serial_port_list_->addItem("OpenPort 2.0");
    window.serial_port_list_->setCurrentIndex(0);
    QTemporaryDir roms;
    const QString romPath = writeRom(roms, "test.bin", '\x5a', 16);
    ASSERT_TRUE(!romPath.isEmpty());
    ModalDriver openDriver{QString()};
    openDriver.start();
    ASSERT_EQ(window.open_calibration_file(romPath), 0);
    openDriver.stop();
    ASSERT_EQ(openDriver.missingDefinitionPromptCount(), 1);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can_checksum_na"));

    ModalDriver operationDriver{QString()};
    operationDriver.start();
    ASSERT_EQ(startEcuOperations(window, command), 0);
    operationDriver.stop();

    ASSERT_TRUE(!operationDriver.timedOut());
    ASSERT_EQ(operationDriver.checksumWarningCount(), 1);
    ASSERT_TRUE(std::ranges::all_of(services.calibrations.Find(window.calibrations_.front().id)->Rom(),
                                    [](auto byte) { return byte == 0x5a; }));
    ASSERT_TRUE(!window.vbatt_timer_->isActive());
}

TEST_P(CancellingTheChecksumWarningStopsVoltagePollingParameters, cancellingTheChecksumWarningStopsVoltagePolling)
{
    ASSERT_NO_FATAL_FAILURE(check_cancellingTheChecksumWarningStopsVoltagePolling(GetParam().command));
}

void MainWindowTest::check_definitionlessOpenPromptsOnceAndAppliesPlaceholders()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTemporaryDir roms;
    const QString path = writeRom(roms, "a.bin", '\x11');

    ModalDriver driver{QString()};
    driver.start();
    ASSERT_EQ(window.open_calibration_file(path), 0);
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
              fastecu::ui::session_key_text(window.calibrations_.front().id));
}

TEST_F(MainWindowTest, definitionlessOpenPromptsOnceAndAppliesPlaceholders)
{
    ASSERT_NO_FATAL_FAILURE(check_definitionlessOpenPromptsOnceAndAppliesPlaceholders());
}

void MainWindowTest::check_closingAMiddleRomKeepsLaterRomsAddressable()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
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
    QTreeWidget *files = window.ui_->calibrationFilesTreeWidget;
    const QString cKey = files->topLevelItem(2)->text(2);

    for (int i = 0; i < files->topLevelItemCount(); ++i)
    {
        files->topLevelItem(i)->setSelected(i == 1);
    }
    window.close_calibration();

    ASSERT_EQ(window.calibrations_.size(), std::size_t{2});
    ASSERT_EQ(services.calibrations.Ids(), (std::vector{a, c}));
    ASSERT_EQ(files->topLevelItemCount(), 2);
    ASSERT_EQ(files->topLevelItem(1)->text(2), cKey); // not renumbered
    ASSERT_TRUE(services.calibrations.Find(c) != nullptr);
    ASSERT_EQ(services.calibrations.Find(c)->Source().display_name, std::string{"c.bin"});
    ASSERT_EQ(services.calibrations.Find(c)->Rom()[0], std::uint8_t{0x0c});
}

TEST_F(MainWindowTest, closingAMiddleRomKeepsLaterRomsAddressable)
{
    ASSERT_NO_FATAL_FAILURE(check_closingAMiddleRomKeepsLaterRomsAddressable());
}

void MainWindowTest::check_windowsOfAClosedRomAreInert()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
    driver.stop();
    const auto b = window.calibrations_.at(1).id;
    window.close_calibration(); // b is selected after its open
    ASSERT_TRUE(services.calibrations.Find(b) == nullptr);

    const QString stale = fastecu::ui::session_key_text(b) + ",0,Idle";
    auto *content = new QWidget;
    QMdiSubWindow *sub = window.ui_->mdiArea->addSubWindow(content);
    sub->setObjectName(stale);
    content->setObjectName(stale);
    window.ui_->mdiArea->setActiveSubWindow(sub);
    QObject destroyedWindow;
    destroyedWindow.setObjectName(stale);

    window.set_maptablewidget_items();
    window.selectable_combobox_item_changed("anything");
    window.checkbox_state_changed(2);
    window.close_calibration_map(&destroyedWindow);

    ASSERT_EQ(window.calibrations_.size(), std::size_t{1});
    ASSERT_EQ(window.ui_->calibrationFilesTreeWidget->topLevelItemCount(), 1);
}

TEST_F(MainWindowTest, windowsOfAClosedRomAreInert)
{
    ASSERT_NO_FATAL_FAILURE(check_windowsOfAClosedRomAreInert());
}

void MainWindowTest::check_hexEditorOutlivesItsRom()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
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
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
    driver.stop();
    const QString aKey = fastecu::ui::session_key_text(window.calibrations_.at(0).id);
    const QString bKey = fastecu::ui::session_key_text(window.calibrations_.at(1).id);
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

    window.close_calibration();

    QStringList remaining;
    for (QMdiSubWindow *sub : window.ui_->mdiArea->subWindowList())
    {
        remaining << sub->objectName();
    }
    ASSERT_EQ(remaining, QStringList{bKey + ",0,Z"});
}

TEST_F(MainWindowTest, closingARomClosesAllOfItsWindows)
{
    ASSERT_NO_FATAL_FAILURE(check_closingARomClosesAllOfItsWindows());
}

void MainWindowTest::check_viewStateIsKeptPerRom()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    QTemporaryDir roms;
    ModalDriver driver{QString()};
    driver.start();
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
    ASSERT_EQ(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
    driver.stop();
    QTreeWidget *files = window.ui_->calibrationFilesTreeWidget;
    QTreeWidget *data = window.ui_->calibrationDataTreeWidget;
    const auto selectRom = [&](int row)
    {
        for (int i = 0; i < files->topLevelItemCount(); ++i)
        {
            files->topLevelItem(i)->setSelected(i == row);
        }
        window.calibration_files_treewidget_item_selected(files->topLevelItem(row));
    };

    selectRom(0);
    window.calibration_data_treewidget_item_expanded(data->topLevelItem(0)); // ROM Info
    selectRom(1);
    ASSERT_TRUE(!data->topLevelItem(0)->isExpanded());
    selectRom(0);
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
    window.status_bar_ecu_label_->setText("stale");
    fastecu::calibration::CalibrationSession session(
        fastecu::calibration::SessionId{41},
        fastecu::calibration::SessionContents{
            .source = {.display_name = "d.bin", .path = "/d.bin"},
            .rom = std::vector<std::uint8_t>(16, 0),
            .definition =
                fastecu::calibration::ResolvedDefinition{
                    .id = "D", .definition = {.format = fastecu::definition::DefinitionFormat::kEcuFlash}},
        });
    ASSERT_EQ(fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(session), fastecu::ui::RomInfoRow::kFlashMethod),
              QString(""));

    // The 16-byte image draws the checksum command's bad-size notice, which
    // the driver accepts.
    const std::optional<fastecu::ui::PreparedWrite> prepared =
        window.calibration_operations_->prepare_write(&session, "/kernels/");
    driver.stop();

    ASSERT_TRUE(!driver.timedOut());
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(window.status_bar_ecu_label_->text().toStdString(), std::string{"Denso SH7058 K-Line "});
    EXPECT_EQ(services.config.SelectedVehicle()->make, std::string{"Nissan"});
    EXPECT_EQ(session.Protocol().flash_method, std::string{"sub_ecu_denso_sh7058"});
    EXPECT_EQ(fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(session), fastecu::ui::RomInfoRow::kFlashMethod)
                  .toStdString(),
              std::string{"sub_ecu_denso_sh7058"});
    EXPECT_EQ(session.Protocol().mcu_type, std::string{"SH7058"});
    EXPECT_EQ(prepared->kernel_path, std::string{"/kernels/test-kernel.bin"});
    EXPECT_EQ(prepared->display_filename, std::string{"d.bin"});
    EXPECT_THAT(prepared->image, ::testing::ElementsAreArray(session.Rom()));
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
    services.config.Settings().primary_definition_base = "ecuflash";
    services.config.Settings().use_ecuflash_definitions = "enabled";
    services.config.Settings().ecuflash_definition_files_directory = files.path().toStdString();
    ASSERT_TRUE(
        services.definition_catalogs.RefreshIndex(fastecu::definition::DefinitionFormat::kEcuFlash).has_value());
    const QString path = files.path() + "/save.bin";
    const auto opened = services.calibrations.AdoptReadImage({
        .rom = bytes::Bytes(1024UZ * 1024, 0),
        .filename = path.toStdString(),
        .rom_id = "SAVE",
        .protocol_name = "sub_ecu_denso_sh7058",
    });
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(window.add_calibration(opened->id));
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    auto *session = services.calibrations.Find(opened->id);
    ASSERT_TRUE(session != nullptr);
    ASSERT_TRUE(session->Definition() != nullptr);
    ASSERT_TRUE(session->WriteBytes(0, bytes::Bytes{1}).has_value());
    const bytes::Bytes original(session->Rom().begin(), session->Rom().end());
    // The expected bytes come from the backend dispatcher, independently of
    // the coordinator's selection plumbing.
    const VehicleSpec& vehicle = *services.config.SelectedVehicle();
    const fastecu::checksum::ChecksumCorrectionOutcome correction = fastecu::checksum::ApplyChecksumCorrection(
        original, {
                      .make = std::string(vehicle.make),
                      .checksum_flag = std::string(fastecu::config::ChecksumFlag(vehicle.protocol->checksum)),
                      .flash_method = std::string(vehicle.protocol->name),
                      .mcu_type = session->Protocol().mcu_type,
                      .rom_id = session->Protocol().rom_id,
                  });
    ASSERT_EQ(correction.status, fastecu::checksum::ChecksumCorrectionOutcome::Status::kFamilyRan);
    ASSERT_TRUE(correction.family_result.has_value());
    ASSERT_TRUE(correction.family_result->Ok());
    const bytes::Bytes corrected = correction.family_result->rom_data;
    ASSERT_TRUE(corrected != original);
    ASSERT_TRUE(session->Dirty());

    window.save_calibration_file();
    EXPECT_THAT(services.file_repository.Read(path.toStdString()), fastecu::testing::IsOkAnd(corrected));
    ASSERT_TRUE(std::ranges::equal(session->Rom(), original));
    ASSERT_TRUE(!session->Dirty());
    ASSERT_TRUE(session->WriteBytes(0, bytes::Bytes{2}).has_value());
    const bytes::Bytes edited(session->Rom().begin(), session->Rom().end());
    // A directory is a deterministic failed file write on every platform.
    session->MarkSaved(files.path().toStdString());
    ASSERT_TRUE(session->WriteBytes(0, bytes::Bytes{2}).has_value());
    const auto source = session->Source();
    window.save_calibration_file();
    ASSERT_TRUE(session->Source() == source);
    ASSERT_TRUE(session->Dirty());
    ASSERT_TRUE(std::ranges::equal(session->Rom(), edited));
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
    auto *session = services.calibrations.Find(window.calibrations_.front().id);
    ASSERT_TRUE(session != nullptr);
    auto protocol = session->Protocol();
    protocol.mcu_type.clear(); // Unknown MCU: an error line and no checksum dialog.
    session->SetProtocol(protocol);
    ASSERT_NO_FATAL_FAILURE(selectSubaruProtocol(window, "sub_ecu_denso_sh7058"));
    fastecu::testing::SignalRecorder debug{&services.log_channel, &fastecu::ui::LogChannel::LOG_D};
    fastecu::testing::SignalRecorder errors{&services.log_channel, &fastecu::ui::LogChannel::LOG_E};

    window.save_calibration_file();

    EXPECT_THAT(logLines(debug),
                ::testing::ElementsAre(LogLine{"Protocol: sub_ecu_denso_sh7058", true, true},
                                       LogLine{"Make: Subaru", true, true}, LogLine{"Checksum: yes", true, true},
                                       LogLine{"ecuCalDef->FileName: caf\xc3\xa9.bin", true, true},
                                       LogLine{"ecuCalDef->FullFileName: " + session->Source().path, true, true}));
    EXPECT_THAT(logLines(errors), ::testing::ElementsAre(LogLine{"Unknown MCU type: ", true, true}));
}

TEST_F(MainWindowTest, calibrationLogsReachTheLogChannel)
{
    ASSERT_NO_FATAL_FAILURE(check_calibrationLogsReachTheLogChannel());
}

void MainWindowTest::check_saveAsChangesSourceAndTreeOnlyAfterSuccess()
{
    const bool nativeDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs =
        qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDisabled); });
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    QTemporaryDir files;
    const QString originalPath = writeRom(files, "original.bin", '\x11');
    ASSERT_EQ(window.open_calibration_file(originalPath), 0);
    auto *session = services.calibrations.Find(window.calibrations_.front().id);
    ASSERT_TRUE(session != nullptr);
    auto protocol = session->Protocol();
    protocol.mcu_type.clear(); // Unknown MCU preserves bytes without a checksum dialog.
    session->SetProtocol(protocol);
    ASSERT_TRUE(session->WriteBytes(0, bytes::Bytes{9}).has_value());
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
    ASSERT_TRUE(saved.has_value());
    ASSERT_EQ(saved->at(0), std::uint8_t{9});
    ASSERT_EQ(session->Rom()[0], std::uint8_t{9});
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
    const bool nativeDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs =
        qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDisabled); });
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
    QTreeWidgetItem *aRow = window.files_tree_item(a->Id());
    QTreeWidgetItem *bRow = window.files_tree_item(b->Id());
    ASSERT_TRUE(aRow != nullptr);
    ASSERT_TRUE(bRow != nullptr);
    const auto bSource = b->Source();
    const std::string bLabel = bRow->text(0).toStdString();
    ASSERT_NO_FATAL_FAILURE(selectFilesRow(window, 0));
    ASSERT_EQ(window.selected_calibration(), a);

    const QString target = files.path() + "/renamed.bin";
    ASSERT_TRUE(driveSaveAs(window, target, false, false, [&] { selectFilesRow(window, 1); }));

    EXPECT_EQ(a->Source().path, target.toStdString());
    EXPECT_EQ(a->Source().display_name, std::string{"renamed.bin"});
    EXPECT_EQ(aRow->text(0).toStdString(), std::string{"renamed.bin"});
    EXPECT_TRUE(b->Source() == bSource);
    EXPECT_EQ(bRow->text(0).toStdString(), bLabel);
    EXPECT_EQ(window.selected_calibration(), b);
    EXPECT_THAT(services.file_repository.Read(target.toStdString()),
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
    services.config.Settings().primary_definition_base = "ecuflash";
    services.config.Settings().use_ecuflash_definitions = "enabled";
    services.config.Settings().ecuflash_definition_files_directory = files.path().toStdString();
    ASSERT_TRUE(
        services.definition_catalogs.RefreshIndex(fastecu::definition::DefinitionFormat::kEcuFlash).has_value());
    const auto openMap = [&](const QString& name) -> CalibrationMaps *
    {
        const auto opened = services.calibrations.AdoptReadImage({
            .rom = bytes::Bytes(16, 0),
            .filename = name.toStdString(),
            .rom_id = "SELECT",
        });
        if (!opened.has_value() || !window.add_calibration(opened->id))
        {
            return nullptr;
        }
        auto *fileTree = window.ui_->calibrationFilesTreeWidget;
        for (int row = 0; row < fileTree->topLevelItemCount(); ++row)
        {
            fileTree->topLevelItem(row)->setSelected(row == fileTree->topLevelItemCount() - 1);
        }
        window.calibration_files_treewidget_item_selected(fileTree->topLevelItem(fileTree->topLevelItemCount() - 1));
        QTreeWidget *tree = window.ui_->calibrationDataTreeWidget;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
        {
            auto *category = tree->topLevelItem(i);
            if (category->text(0) == "Controls" && category->childCount() != 0)
            {
                tree->setCurrentItem(category->child(0));
                window.calibration_data_treewidget_item_selected(category->child(0));
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
    first->selectable_combobox_item_changed("enabled");

    ASSERT_EQ(services.calibrations.Find(firstId)->Rom()[0], std::uint8_t{1});
    ASSERT_TRUE(services.calibrations.Find(firstId)->Dirty());
    ASSERT_EQ(services.calibrations.Find(secondId)->Rom()[0], std::uint8_t{0});
    ASSERT_TRUE(!services.calibrations.Find(secondId)->Dirty());
    driver.stop();
    ASSERT_TRUE(!driver.timedOut());
}

TEST_F(MainWindowTest, selectableSignalEditsItsEmittingSession)
{
    ASSERT_NO_FATAL_FAILURE(check_selectableSignalEditsItsEmittingSession());
}

void MainWindowTest::check_failedMapDecodeKeepsAnErrorView()
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
    auto *session = services.calibrations.Find(id);
    ASSERT_TRUE(session != nullptr);
    fastecu::definition::RomDefinition definition{.format = fastecu::definition::DefinitionFormat::kEcuFlash};
    definition.scalings.push_back({.name = "Raw"});
    fastecu::definition::CalibrationMap map;
    map.name = "Broken";
    map.category = "Controls";
    map.type = "1D";
    map.address = 1000; // Beyond the opened 16-byte image.
    map.x_size = 1;
    map.y_size = 1;
    map.storage_type = fastecu::definition::StorageType::kUint8;
    map.scaling_name = "Raw";
    definition.maps.push_back(map);
    *session = fastecu::calibration::CalibrationSession(
        id,
        {
            .source = session->Source(),
            .rom = bytes::Bytes(16, 0),
            .definition = fastecu::calibration::ResolvedDefinition{.id = "BAD", .definition = std::move(definition)},
        });
    window.calibration_files_treewidget_item_selected(window.ui_->calibrationFilesTreeWidget->topLevelItem(0));
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
    fastecu::testing::SignalRecorder errors(&window, &MainWindow::LOG_E);
    tree->setCurrentItem(item);
    window.calibration_data_treewidget_item_selected(item);
    ASSERT_EQ(window.ui_->mdiArea->subWindowList().size(), 1);
    auto *view = window.ui_->mdiArea->subWindowList().front()->widget();
    ASSERT_NE(view->findChild<QLabel *>("mapDecodeError"), nullptr);
    EXPECT_FALSE(view->findChild<QTableWidget *>()->isEnabled());
    ASSERT_EQ(window.calibrations_.front().view.open_maps.size(), 1U);
    ASSERT_EQ(item->checkState(0), Qt::Checked);
    ASSERT_TRUE(!errors.snapshot().empty());
}

TEST_F(MainWindowTest, failedMapDecodeKeepsAnErrorView)
{
    ASSERT_NO_FATAL_FAILURE(check_failedMapDecodeKeepsAnErrorView());
}

void MainWindowTest::check_windowPreservesInjectedLoggingFactory()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
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
            protocol->BlockPollUntilCancelled();
            return protocol;
        });
    MainWindow window{services.services()};
    constructorDriver.stop();
    // This test checks ownership, not UI error dialogs. The next test
    // drives actual menu dispatch and checks the selected target.
    QObject::disconnect(&services.logging_engine, nullptr, &window, nullptr);
    auto session =
        fastecu::logging::MakeLoggingSession(fastecu::logging::LoggingProtocolId::kSsm,
                                             {{.id = "rpm",
                                               .address = 0x10,
                                               .length = 1,
                                               .raw_assembly = fastecu::logging::RawAssembly::kUnsignedIntegerDecimal,
                                               .from_byte_expression = "x",
                                               .unit = "rpm",
                                               .decimal_precision = 0}},
                                             {.poll_timeout = std::chrono::milliseconds{50},
                                              .car_silence_miss_threshold = 20,
                                              .reconnect_attempt_threshold = 100,
                                              .reconnect_retry_period = 20});
    ASSERT_TRUE(session);
    ASSERT_TRUE(services.logging_engine.start(
        {.protocol_id = "SSM"}, {.session = std::move(*session), .response_offsets = {0}, .target_is_ecu = false}));
    services.logging_engine.stop();
    ASSERT_TRUE(called);
}

TEST_F(MainWindowTest, windowPreservesInjectedLoggingFactory)
{
    ASSERT_NO_FATAL_FAILURE(check_windowPreservesInjectedLoggingFactory());
}

void MainWindowTest::check_loggingCapturesTargetForEachRun()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.config_session_->Settings().selected_log_protocol = "SSM";
    QAction *action = prepareLogging(window, "SSM");
    std::vector<bool> targets;
    services.logging_engine.registerProtocol(
        "SSM",
        [&targets](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
        {
            targets.push_back(snapshot.target_is_ecu);
            auto protocol = std::make_unique<ScriptedLoggingProtocol>();
            protocol->BlockPollUntilCancelled();
            return protocol;
        });
    for (bool target : {true, false})
    {
        window.ecu_radio_button_->setAutoExclusive(false);
        window.ecu_radio_button_->setChecked(target);
        // trigger() toggles a checkable action, as a click does: start
        // unchecked so the handler sees Logging switched on.
        action->setChecked(false);
        ASSERT_TRUE(triggerMenu(window, kToggleRealtime));
        ASSERT_TRUE(action->isChecked());
        ASSERT_TRUE(window.active_logging_snapshot_.has_value());
        ASSERT_EQ(window.active_logging_snapshot_->target_is_ecu, target);
        services.logging_engine.stop();
    }
    ASSERT_EQ(targets, (std::vector<bool>{true, false}));
}

TEST_F(MainWindowTest, loggingCapturesTargetForEachRun)
{
    ASSERT_NO_FATAL_FAILURE(check_loggingCapturesTargetForEachRun());
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
    [](const ::testing::TestParamInfo<ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationCase>& info)
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
    ASSERT_TRUE(services.config.SelectRow(0).has_value());
    ASSERT_TRUE(services.config.Save().has_value());
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    MainWindow window{services.services()};
    constructorDriver.stop();
    const auto target = services.config.Vehicles()[1];
    std::size_t expected = 1;
    if (protocol)
    {
        for (std::size_t row = 0; row < services.config.Vehicles().size(); ++row)
        {
            if (services.config.Vehicles()[row].protocol == target.protocol)
            {
                expected = row;
            }
        }
    }
    QTimer driver;
    QElapsedTimer deadline;
    bool driven = false;
    bool unexpected = false;
    bool timedOut = false;
    QObject::connect(
        &driver, &QTimer::timeout,
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
            const char *expectedClass = protocol ? "ProtocolSelect" : "VehicleSelect";
            if (!dialog->inherits(expectedClass))
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
                selected = selectText(dialog->findChild<QTreeWidget *>("treeWidget"), qs(target.protocol->name));
            }
            else
            {
                selected = selectText(dialog->findChild<QTreeWidget *>("car_make_tree_widget"), qs(target.make)) &&
                           selectText(dialog->findChild<QTreeWidget *>("car_model_tree_widget"), qs(target.model));
                auto *versions = dialog->findChild<QTreeWidget *>("car_version_tree_widget");
                selected = selected && versions != nullptr;
                bool rowFound = false;
                if (versions)
                {
                    for (int i = 0; i < versions->topLevelItemCount(); ++i)
                    {
                        auto *item = versions->topLevelItem(i);
                        if (item->text(12) == "1")
                        {
                            versions->setCurrentItem(item);
                            rowFound = true;
                            break;
                        }
                    }
                }
                selected = selected && rowFound;
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
    ASSERT_TRUE(!timedOut);
    ASSERT_EQ(*services.config.SelectedRow(), accept ? expected : std::size_t{0});
    TestServices reread{root.path()};
    ASSERT_TRUE(reread.config_status.has_value());
    ASSERT_EQ(*reread.config.SelectedRow(), accept ? expected : std::size_t{0});
}

TEST_P(ChooserDialogsApplyAcceptedChoicesAndIgnoreCancellationParameters,
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
    services.config.Settings().romraider_definition_files = {"/first.xml", "/middle.xml", "/last.xml"};
    ASSERT_TRUE(services.config.Save().has_value());
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
    window.ecu_definition_manager();
    driver.stop();
    ASSERT_TRUE(driven);
    ASSERT_TRUE(!unexpected);
    ASSERT_TRUE(unchangedWithoutSelection);
    ASSERT_EQ(displayed, (QStringList{"/first.xml", "/last.xml"}));
    const std::vector<std::string> expected{"/first.xml", "/last.xml"};
    ASSERT_EQ(services.config.Settings().romraider_definition_files, expected);
    TestServices reread{root.path()};
    ASSERT_TRUE(reread.config_status.has_value());
    ASSERT_EQ(reread.config.Settings().romraider_definition_files, expected);
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
    services.config.Settings().window_width = "900";
    services.config.Settings().window_height = "700";
    ASSERT_TRUE(services.config.Save().has_value());
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_EQ(window.size(), QSize(900, 700));
    window.show();
    window.resize(950, 750);
    QCoreApplication::processEvents();
    ASSERT_EQ(window.size(), QSize(950, 750));
    TestServices resized{root.path()};
    ASSERT_TRUE(resized.config_status.has_value());
    ASSERT_EQ(resized.config.Settings().window_width, std::string("950"));
    ASSERT_EQ(resized.config.Settings().window_height, std::string("750"));
    window.showMaximized();
    QCoreApplication::processEvents();
    ASSERT_EQ(services.config.Settings().window_width, std::string("maximized"));
    ASSERT_EQ(services.config.Settings().window_height, std::string("maximized"));
    TestServices maximized{root.path()};
    ASSERT_TRUE(maximized.config_status.has_value());
    ASSERT_EQ(maximized.config.Settings().window_width, std::string("maximized"));
    window.showNormal();
    QCoreApplication::processEvents();
    ASSERT_EQ(services.config.Settings().window_width, std::to_string(window.width()));
    ASSERT_EQ(services.config.Settings().window_height, std::to_string(window.height()));
    TestServices restored{root.path()};
    ASSERT_TRUE(restored.config_status.has_value());
    ASSERT_EQ(restored.config.Settings().window_width, services.config.Settings().window_width);
    ASSERT_EQ(restored.config.Settings().window_height, services.config.Settings().window_height);
}

TEST_F(MainWindowTest, numericWindowGeometryRestoresAndPersistsAcrossWindowStates)
{
    ASSERT_NO_FATAL_FAILURE(check_numericWindowGeometryRestoresAndPersistsAcrossWindowStates());
}

void MainWindowTest::check_acceptedVehicleChoiceSelectsTheRowAndSavesIt()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    const std::string flashTransport = services.config.Settings().selected_flash_transport;
    const std::string logTransport = services.config.Settings().selected_log_transport;

    window.apply_vehicle_choice(QDialog::Accepted, 1);

    ASSERT_EQ(*services.config.SelectedRow(), std::size_t{1});
    ASSERT_EQ(services.config.Settings().selected_log_protocol, std::string("SSM"));
    ASSERT_EQ(services.config.Settings().selected_flash_transport, flashTransport);
    ASSERT_EQ(services.config.Settings().selected_log_transport, logTransport);

    // Saved: a fresh session over the same root restores row 1.
    QtEventSink rereadEvents;
    fastecu::config::ConfigSession reread{kWindowCatalog, services.file_system, services.resource_bundle,
                                          services.file_repository, rereadEvents};
    ASSERT_TRUE(reread.Initialize(config_root_->path().toStdString(), kTestApplication.version).has_value());
    ASSERT_EQ(reread.Settings().selected_vehicle_id, std::string(kWindowVehicles[1].id));
}

TEST_F(MainWindowTest, acceptedVehicleChoiceSelectsTheRowAndSavesIt)
{
    ASSERT_NO_FATAL_FAILURE(check_acceptedVehicleChoiceSelectsTheRowAndSavesIt());
}

void MainWindowTest::check_cancelledVehicleChoiceChangesNothing()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    const auto before = services.config.Settings();

    window.apply_vehicle_choice(QDialog::Rejected, 1);

    ASSERT_TRUE(services.config.Settings() == before);
}

TEST_F(MainWindowTest, cancelledVehicleChoiceChangesNothing)
{
    ASSERT_NO_FATAL_FAILURE(check_cancelledVehicleChoiceChangesNothing());
}

void MainWindowTest::check_acceptedProtocolChoiceSelectsTheLastMatchingRow()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
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

    window.apply_protocol_choice(QDialog::Accepted, std::string("sub_ecu_denso_sh7058"));

    ASSERT_EQ(*services.config.SelectedRow(), last);
}

TEST_F(MainWindowTest, acceptedProtocolChoiceSelectsTheLastMatchingRow)
{
    ASSERT_NO_FATAL_FAILURE(check_acceptedProtocolChoiceSelectsTheLastMatchingRow());
}

void MainWindowTest::check_romFlashMethodSelectsTheLastMatchingRow()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.update_protocol_info("sub_ecu_denso_sh7058");

    ASSERT_EQ(*services.config.SelectedRow(), std::size_t{9}); // rows 3, 8, 9 match; the last wins
    ASSERT_EQ(services.config.SelectedVehicle()->make, std::string("Nissan"));
}

TEST_F(MainWindowTest, romFlashMethodSelectsTheLastMatchingRow)
{
    ASSERT_NO_FATAL_FAILURE(check_romFlashMethodSelectsTheLastMatchingRow());
}

void MainWindowTest::check_unmatchedRomFlashMethodChangesNothing()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    const auto before = services.config.Settings();
    window.update_protocol_info("no_such_protocol");

    ASSERT_TRUE(services.config.Settings() == before);
}

TEST_F(MainWindowTest, unmatchedRomFlashMethodChangesNothing)
{
    ASSERT_NO_FATAL_FAILURE(check_unmatchedRomFlashMethodChangesNothing());
}

void MainWindowTest::check_restoreLoggingUiStateUnchecksLogging()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
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
    ASSERT_NO_FATAL_FAILURE(check_restoreLoggingUiStateUnchecksLogging());
}

void MainWindowTest::check_setRealtimeStateChecksAndUnchecksLogging()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();

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
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();

    QAction *logging = menuAction(window, kToggleRealtime);
    QAction *connectAction = menuAction(window, kConnectToEcu);
    QAction *disconnectAction = menuAction(window, kDisconnectFromEcu);
    ASSERT_NE(logging, nullptr);
    ASSERT_NE(connectAction, nullptr);
    ASSERT_NE(disconnectAction, nullptr);

    window.connection_presentation_.set_controls_locked(true);
    EXPECT_FALSE(logging->isEnabled());
    EXPECT_FALSE(connectAction->isEnabled());
    EXPECT_TRUE(disconnectAction->isEnabled());

    window.connection_presentation_.set_controls_locked(false);
    EXPECT_TRUE(logging->isEnabled());
    EXPECT_TRUE(connectAction->isEnabled());
    EXPECT_TRUE(disconnectAction->isEnabled());
}

TEST_F(MainWindowTest, identificationDisablesLoggingAndConnectButNotDisconnect)
{
    ASSERT_NO_FATAL_FAILURE(check_identificationDisablesLoggingAndConnectButNotDisconnect());
}

void MainWindowTest::check_logToFileActionDrivesWriteDatalogToFile()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();

    QAction *logToFile = menuAction(window, kLogToFile);
    ASSERT_NE(logToFile, nullptr);
    ASSERT_TRUE(logToFile->isCheckable());

    logToFile->setChecked(true);
    window.toggle_log_to_file();
    EXPECT_TRUE(window.write_datalog_to_file_);

    logToFile->setChecked(false);
    window.toggle_log_to_file();
    EXPECT_FALSE(window.write_datalog_to_file_);
}

TEST_F(MainWindowTest, logToFileActionDrivesWriteDatalogToFile)
{
    ASSERT_NO_FATAL_FAILURE(check_logToFileActionDrivesWriteDatalogToFile());
}

void MainWindowTest::check_flashActionsFollowTheSelectedProtocolsCapabilities()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
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
    window.set_flash_arrow_state();
    ASSERT_TRUE(readAction->isEnabled());
    ASSERT_TRUE(testWriteAction->isEnabled());
    ASSERT_TRUE(writeAction->isEnabled());

    // ...and one without test write disables only that action.
    ASSERT_NO_FATAL_FAILURE(selectProtocol(window, "sub_ecu_denso_sh7058"));
    window.set_flash_arrow_state();
    ASSERT_TRUE(readAction->isEnabled());
    ASSERT_TRUE(!testWriteAction->isEnabled());
    ASSERT_TRUE(writeAction->isEnabled());
}

TEST_F(MainWindowTest, flashActionsFollowTheSelectedProtocolsCapabilities)
{
    ASSERT_NO_FATAL_FAILURE(check_flashActionsFollowTheSelectedProtocolsCapabilities());
}

void MainWindowTest::check_loggingUsesTheSessionLogProtocol()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
    prepareLogging(window, "CDBG");
    services.logging_engine.registerProtocol("CDBG",
                                             [](const fastecu::desktop::logging::DesktopLoggingSnapshot&)
                                             {
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->BlockPollUntilCancelled();
                                                 return protocol;
                                             });
    window.config_session_->Settings().selected_log_protocol = "CDBG";

    ModalDriver driver{QString()};
    driver.start();
    window.continue_start_logging();
    driver.stop();
    services.logging_engine.stop();

    ASSERT_EQ(window.active_log_value_protocol_filter_, QString("CDBG"));
}

TEST_F(MainWindowTest, loggingUsesTheSessionLogProtocol)
{
    ASSERT_NO_FATAL_FAILURE(check_loggingUsesTheSessionLogProtocol());
}

void MainWindowTest::check_selectedSerialPortIsEmptyWithoutPorts()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.serial_ports_.clear();
    window.serial_port_list_->clear();
    ASSERT_EQ(window.selected_serial_port(), QString());
    window.serial_ports_ = {"ttyUSB0"};
    window.serial_port_list_->addItem("ttyUSB0");
    ASSERT_EQ(window.selected_serial_port(), QString("ttyUSB0"));
}

TEST_F(MainWindowTest, selectedSerialPortIsEmptyWithoutPorts)
{
    ASSERT_NO_FATAL_FAILURE(check_selectedSerialPortIsEmptyWithoutPorts());
}

void MainWindowTest::check_dtcWindowWithoutAPortWarnsInsteadOfCrashing()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
    window.serial_ports_.clear();
    window.serial_port_list_->clear();
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
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    TestServices services{root.path()};
    ASSERT_TRUE(services.config_status.has_value());
    // A fresh root has no saved vehicle; the startup gate would ask for one.
    ASSERT_TRUE(services.config.SelectRow(0).has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    fastecu::testing::SignalRecorder errors{&window, &MainWindow::LOG_E};
    const QString configFile = QString::fromStdString(services.config.ProvisionedPaths().config_file);
    ASSERT_TRUE(QFile::remove(configFile));
    ASSERT_TRUE(QDir().mkpath(configFile));

    services.config.Settings().toolbar_iconsize = "48";
    window.save_settings();
    window.save_settings();
    window.save_settings();
    ASSERT_EQ(errors.count(), 1U);
    ASSERT_TRUE(std::get<0>(errors.snapshot().front()).contains(configFile));
    ASSERT_EQ(services.config.Settings().toolbar_iconsize, std::string("48"));

    ASSERT_TRUE(QDir().rmdir(configFile));
    window.save_settings();
    ASSERT_EQ(errors.count(), 1U);
    QFile saved{configFile};
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    ASSERT_TRUE(saved.readAll().contains(R"(data="48")"));
    saved.close();
    ASSERT_TRUE(QFile::remove(configFile));
    ASSERT_TRUE(QDir().mkpath(configFile));
    window.save_settings();
    ASSERT_EQ(errors.count(), 2U);
}

TEST_F(MainWindowTest, repeatedSaveFailuresLogOnceUntilASuccess)
{
    ASSERT_NO_FATAL_FAILURE(check_repeatedSaveFailuresLogOnceUntilASuccess());
}

void MainWindowTest::check_biuWindowRemembersTheOpenedPort()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    ON_CALL(*services.fake, get_openedSerialPort()).WillByDefault(::testing::Return(QString("ttyUSB0")));
    window.previous_serial_port_.clear();
    window.config_session_->Settings().serial_port = "none";
    window.save_settings();
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
    ASSERT_NO_FATAL_FAILURE(check_biuWindowRemembersTheOpenedPort());
}

void MainWindowTest::check_disconnectReturnsTheAdapterToIdle()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
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
    ASSERT_TRUE(window.serial_port_list_->isEnabled());
}

TEST_F(MainWindowTest, disconnectReturnsTheAdapterToIdle)
{
    ASSERT_NO_FATAL_FAILURE(check_disconnectReturnsTheAdapterToIdle());
}

void MainWindowTest::check_connectOnAnotherMakeDisconnectsWithoutIdentifying()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Mitsubishi", "K-Line"));
    EXPECT_CALL(*services.fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(*services.fake, set_serial_port_parity(0)).Times(::testing::AtLeast(1));

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
    ASSERT_NO_FATAL_FAILURE(check_connectOnAnotherMakeDisconnectsWithoutIdentifying());
}

void MainWindowTest::check_subaruKlineConnectIdentifiesOffTheUiThread()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    std::atomic<bool> readOffUiThread = false;
    EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
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
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return !window.connection_coordinator_->identifying(); },
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
    ASSERT_NO_FATAL_FAILURE(check_subaruKlineConnectIdentifiesOffTheUiThread());
}

void MainWindowTest::check_subaruConnectThatNeverAnswersDisconnectsAndRestoresControls()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
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
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return !window.connection_coordinator_->identifying(); },
                                             std::chrono::milliseconds(15000)));
    ASSERT_TRUE(!window.ecu_init_complete_);
    ASSERT_TRUE(window.log_transport_list_->isEnabled());
    ASSERT_TRUE(window.serial_port_list_->isEnabled());
    ASSERT_TRUE(window.ecu_radio_button_->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button_->isEnabled());
}

TEST_F(MainWindowTest, subaruConnectThatNeverAnswersDisconnectsAndRestoresControls)
{
    ASSERT_NO_FATAL_FAILURE(check_subaruConnectThatNeverAnswersDisconnectsAndRestoresControls());
}

void MainWindowTest::check_disconnectDuringIdentificationCancelsAndDropsTheResult()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));

    EXPECT_CALL(*services.fake, read_serial_data(::testing::_)).WillOnce(::testing::Return(kEcuInit));
    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    // Leave a successful completion queued on the UI thread before cancelling.
    ASSERT_TRUE(window.identify_launcher_->wait_for_worker(std::chrono::milliseconds(5000)));
    ASSERT_TRUE(triggerMenu(window, kDisconnectFromEcu));
    ASSERT_TRUE(!window.connection_coordinator_->identifying());
    ASSERT_TRUE(window.log_transport_list_->isEnabled());
    ASSERT_TRUE(window.serial_port_list_->isEnabled());
    ASSERT_TRUE(window.ecu_radio_button_->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button_->isEnabled());
    fastecu::testing::process_events_for(
        std::chrono::milliseconds(200)); // any completion already queued must be dropped
    ASSERT_TRUE(!window.ecu_init_complete_);
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
    window.vbatt_timer_->stop();
    const auto cfg = QString::fromStdString(services.config.EffectivePaths().logger_file);
    window.ecuid_ = "MODEL_TEST";
    installLoggingFixture(
        window,
        {.parameters = {{.protocol = "SSM", .id = "rpm", .ecu_byte_index = "0", .ecu_bit = "0", .enabled = true}},
         .switches = {{.protocol = "SSM", .id = "flag", .ecu_byte_index = "5", .enabled = true}}},
        {.protocol = "SSM", .gauge_ids = {"old"}, .lower_panel_ids = {"old"}, .switch_ids = {"old"}});
    const auto previous = window.logger_model_->Selection();
    ASSERT_TRUE(QFile::remove(cfg));
    window.load_logger_selection();
    ASSERT_TRUE(window.logger_model_->Selection() == previous);
    window.logger_model_->SetSelection({.protocol = "SSM", .gauge_ids = {"operator-edit", "unresolved"}});
    const auto edited = window.logger_model_->Selection();
    window.save_logger_selection();
    ASSERT_TRUE(window.logger_model_->Selection() == edited);
    ASSERT_TRUE(writeTextFile(cfg, "<config><logger/></config>"));
    window.logger_model_->SetParameterSupported("SSM", "rpm", false);
    window.load_logger_selection();
    ASSERT_TRUE(window.logger_model_->Selection().gauge_ids.empty());
    ASSERT_EQ(window.logger_model_->Selection().switch_ids, (std::vector<std::string>{"flag"}));
    ASSERT_TRUE(!window.logger_model_->ParameterSupported("SSM", "rpm"));
    ASSERT_TRUE(writeTextFile(
        cfg,
        R"(<config><logger><ecu id="MODEL_TEST"><protocol id="SSM"><parameters><gauges><parameter id="unknown"/></gauges><lower_panel><parameter id="rpm"/></lower_panel></parameters><switches><switch id="flag"/></switches></protocol></ecu></logger></config>)"));
    window.load_logger_selection();
    ASSERT_EQ(window.logger_model_->Selection().gauge_ids, (std::vector<std::string>{"unknown"}));
    ASSERT_TRUE(!window.logger_model_->ParameterSupported("SSM", "rpm"));
    // A valid capability byte updates parameters; missing switch bytes retain flags.
    window.parse_log_value_list(frame({0, 0, 0, 0, 0, 1}), "SSM");
    ASSERT_TRUE(window.logger_model_->ParameterSupported("SSM", "rpm"));
    ASSERT_TRUE(window.logger_model_->SwitchSupported("SSM", "flag"));
    ASSERT_TRUE(window.logger_model_->Definition().parameters.front().enabled);
    window.save_logger_selection();
    const auto stored = services.logger_definitions.LoadSelection(cfg.toStdString(), "MODEL_TEST");
    ASSERT_TRUE(stored.has_value());
    ASSERT_TRUE(stored->has_value());
    ASSERT_TRUE(**stored == window.logger_model_->Selection());
    // Missing definitions clear stale IDs after a successful read and never persist defaults.
    installLoggingFixture(window, {}, {.protocol = "SSM", .lower_panel_ids = {"stale"}});
    ASSERT_TRUE(writeTextFile(cfg, "<config><logger/></config>"));
    window.load_logger_selection();
    ASSERT_TRUE(window.logger_model_->Selection().lower_panel_ids.empty());
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
    services.config.Settings().romraider_logger_definition_file = "/missing/logger.xml";
    MainWindow window{services.services()};
    ASSERT_TRUE(window.logger_model_->Definition().parameters.empty());
    ASSERT_TRUE(window.logger_model_->Selection().lower_panel_ids.empty());
    ASSERT_TRUE(window.ui_ != nullptr);
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
    ASSERT_EQ(window.ui_->logBoxLayout->count(), 1);
    ASSERT_TRUE(window.logger_values_.set_parameter_value({"SSM", "rpm"}, "123.00"));
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
                         [](const ::testing::TestParamInfo<ChooserDuplicateLabelIdentityCase>& info)
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
    ASSERT_TRUE(window.logger_values_.set_parameter_value({"SSM", "rpm"}, "11.00"));
    ASSERT_TRUE(window.logger_values_.set_parameter_value({"CDBG", "rpm"}, "22.00"));
    auto snapshot = fastecu::desktop::logging::make_desktop_logging_snapshot(
        *window.logger_model_, fastecu::logging::LoggingProtocolId::kCdbg, "CDBG",
        {.poll_timeout = std::chrono::milliseconds{50},
         .car_silence_miss_threshold = 20,
         .reconnect_attempt_threshold = 100,
         .reconnect_retry_period = 20});
    ASSERT_TRUE(snapshot.has_value());
    window.active_logging_snapshot_ = *snapshot;
    window.protocol_ = "SSM"; // active run, not mutable UI choice, owns CSV protocol
    ASSERT_TRUE(QDir().mkpath(QString::fromStdString(services.config.EffectivePaths().datalog_files_directory)));
    window.write_datalog_to_file_ = true;
    window.log_to_file();
    window.log_to_file();
    window.datalog_file_outstream_.flush();
    QFile csv{window.datalog_file_.fileName()};
    ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
    const auto content = csv.readAll();
    ASSERT_TRUE(content.startsWith("Time,,Correct CDBG,,,\n"));
    ASSERT_TRUE(content.contains(",,22.00,,,\n"));
    ASSERT_TRUE(!content.contains("Wrong SSM"));
    ASSERT_TRUE(!content.contains("11.00"));
    window.datalog_file_.close();
}

TEST_F(MainWindowTest, csvSharedIdProtocolIdentity)
{
    ASSERT_NO_FATAL_FAILURE(check_csvSharedIdProtocolIdentity());
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
                         [](const ::testing::TestParamInfo<LoggingStartWaitsForIdentificationCase>& info)
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
void MainWindowTest::check_loggingStartWaitsForIdentification(bool targetIsEcu)
{

    QSemaphore responseGate;
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    ASSERT_TRUE(services.fake != nullptr);
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "iso15765"));
    window.protocol_ = "SSM";
    (targetIsEcu ? window.ecu_radio_button_ : window.tcu_radio_button_)->setChecked(true);
    EXPECT_CALL(*services.fake,
                write_serial_data_echo_check(frame({0x00, 0x00, 0x07, targetIsEcu ? 0xE0 : 0xE1, 0x22, 0xF1, 0x82})));
    EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
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
    services.logging_engine.registerProtocol(
        "SSM",
        [&window, &targetFrozenInContinuation](const fastecu::desktop::logging::DesktopLoggingSnapshot&)
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
    ASSERT_TRUE(!window.active_logging_snapshot_.has_value()); // still identifying
    (targetIsEcu ? window.tcu_radio_button_ : window.ecu_radio_button_)->click();
    responseGate.release();
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return window.active_logging_snapshot_.has_value(); },
                                             std::chrono::milliseconds(5000)));
    ASSERT_EQ(window.ecuid_, QString("123456789A"));
    ASSERT_TRUE(window.logger_model_->ParameterSupported("SSM", "rpm"));
    ASSERT_TRUE(window.active_logging_snapshot_.has_value());
    ASSERT_EQ(window.active_logging_snapshot_->target_is_ecu, targetIsEcu);
    ASSERT_TRUE(targetFrozenInContinuation);
    ASSERT_TRUE(window.ecu_radio_button_->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button_->isEnabled());
    services.logging_engine.stop();
}

TEST_P(LoggingStartWaitsForIdentificationParameters, loggingStartWaitsForIdentification)
{
    ASSERT_NO_FATAL_FAILURE(check_loggingStartWaitsForIdentification(GetParam().target_is_ecu));
}

void MainWindowTest::check_batterySamplingDoesNotUseTheFacadeDuringIdentification()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
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
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    auto window = std::make_unique<MainWindow>(services.services());
    constructorDriver.stop();
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
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    services.logging_engine.registerProtocol("SSM",
                                             [](const fastecu::desktop::logging::DesktopLoggingSnapshot&)
                                             {
                                                 auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                                                 protocol->BlockPollUntilCancelled();
                                                 return protocol;
                                             });
    auto session =
        fastecu::logging::MakeLoggingSession(fastecu::logging::LoggingProtocolId::kSsm,
                                             {{.id = "rpm",
                                               .address = 0x10,
                                               .length = 1,
                                               .raw_assembly = fastecu::logging::RawAssembly::kUnsignedIntegerDecimal,
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
    window.logging_state_ = true;
    ASSERT_TRUE(triggerMenu(window, kConnectToEcu));
    ASSERT_TRUE(window.connection_coordinator_->identifying());
    ASSERT_TRUE(!services.logging_engine.isRunning());
    ASSERT_TRUE(!window.logging_state_);
}

TEST_F(MainWindowTest, connectStopsAnActiveLoggingWorkerBeforeIdentification)
{
    ASSERT_NO_FATAL_FAILURE(check_connectStopsAnActiveLoggingWorkerBeforeIdentification());
}

struct ConnectionEntryPointsStopIdentificationCase
{
    std::string name;
    QString entry_point;
};
std::vector<ConnectionEntryPointsStopIdentificationCase> connectionEntryPointsStopIdentificationRows()
{
    std::vector<ConnectionEntryPointsStopIdentificationCase> rows;

    for (const char *name : {"log_transport_changed", "check_serial_ports", "open_serial_port", "show_dtc_window",
                             "show_subaru_biu_window", "show_terminal_window"})
    {
        rows.push_back(ConnectionEntryPointsStopIdentificationCase{name, QString::fromLatin1(name)});
    }

    return rows;
}
class ConnectionEntryPointsStopIdentificationParameters
    : public MainWindowTest,
      public ::testing::WithParamInterface<ConnectionEntryPointsStopIdentificationCase>
{
};
INSTANTIATE_TEST_SUITE_P(Rows, ConnectionEntryPointsStopIdentificationParameters,
                         ::testing::ValuesIn(connectionEntryPointsStopIdentificationRows()),
                         [](const ::testing::TestParamInfo<ConnectionEntryPointsStopIdentificationCase>& info)
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
void MainWindowTest::check_connectionEntryPointsStopIdentification(QString entryPoint)
{

    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    bool cancelled = false;
    window.connect_to_ecu([&cancelled](bool connected) { cancelled = !connected; });
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
    if (entryPoint == "show_dtc_window")
    {
        window.show_dtc_window();
    }
    else if (entryPoint == "show_subaru_biu_window")
    {
        window.show_subaru_biu_window();
    }
    else if (entryPoint == "show_terminal_window")
    {
        window.show_terminal_window();
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
    fastecu::testing::process_events_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(!window.ecu_init_complete_);
}

TEST_P(ConnectionEntryPointsStopIdentificationParameters, connectionEntryPointsStopIdentification)
{
    ASSERT_NO_FATAL_FAILURE(check_connectionEntryPointsStopIdentification(GetParam().entry_point));
}

void MainWindowTest::check_nestedConnectDuringCapabilityNoticeKeepsEachContinuation()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();
    ASSERT_NO_FATAL_FAILURE(prepareConnect(window, *services.fake, "Subaru", "K-Line"));
    EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
        .WillOnce(::testing::Return(kEcuInit))
        .WillRepeatedly(::testing::Return(QByteArray{}));
    std::optional<bool> firstResult;
    std::optional<bool> secondResult;
    window.connect_to_ecu([&firstResult](bool connected) { firstResult = connected; });
    ASSERT_TRUE(window.identify_launcher_->wait_for_worker(std::chrono::milliseconds(5000)));
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
                                     window.connect_to_ecu([&secondResult](bool connected)
                                                           { secondResult = connected; });
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
    ASSERT_TRUE(firstResult.has_value());
    ASSERT_TRUE(!*firstResult);
    ASSERT_TRUE(!secondResult.has_value());
    window.connection_coordinator_->cancel();
    ASSERT_TRUE(secondResult.has_value());
    ASSERT_TRUE(!*secondResult);
    ASSERT_TRUE(window.ecu_radio_button_->isEnabled());
    ASSERT_TRUE(window.tcu_radio_button_->isEnabled());
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
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();

    const std::string actual = fastecu::ui::testing::menu_snapshot(*window.ui_->menubar, *window.ui_->toolBar);
    const char *goldenPath = std::getenv("MAIN_MENU_GOLDEN_PATH");
    ASSERT_NE(goldenPath, nullptr) << "MAIN_MENU_GOLDEN_PATH must be set by the Bazel target's env";
    std::ifstream file(goldenPath, std::ios::binary);
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
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
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
    // BIU communication, Get Encryption Key and WinOLS CSV have no icon by
    // design; any other null icon is a mistyped path.
    EXPECT_EQ(withoutIcon, 8);
}

TEST_F(MainWindowTest, everyIconNamedByTheMenuResolves)
{
    ASSERT_NO_FATAL_FAILURE(check_everyIconNamedByTheMenuResolves());
}

void MainWindowTest::check_noTwoActionsShareAShortcutAndNoneLostItsBinding()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();

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
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
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
    const QString menuCfg = root.path() + "/" + QString::fromStdString(kTestApplication.version) + "/config/menu.cfg";
    ASSERT_TRUE(writeTextFile(menuCfg, "<<< not xml >>>"));

    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{root.path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();

    // The runtime menu loader raised this warning for an unreadable menu.cfg;
    // ModalDriver accepts any box it does not recognise, so check its record.
    for (const QString& text : constructorDriver.acceptedTexts())
    {
        EXPECT_FALSE(text.startsWith(QStringLiteral("Unable to load menu config file"))) << qPrintable(text);
    }
    EXPECT_NE(window.ui_->actionToggleRealtime, nullptr);
    EXPECT_EQ(window.ui_->menubar->findChildren<QMenu *>().size(), 7);
}

TEST_F(MainWindowTest, aStaleOrMalformedMenuCfgIsIgnored)
{
    ASSERT_NO_FATAL_FAILURE(check_aStaleOrMalformedMenuCfgIsIgnored());
}

void MainWindowTest::check_everyMenuActionIsConnectedToTheWindow()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
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
    EXPECT_EQ(probed, 31);
    EXPECT_TRUE(unconnected.isEmpty()) << "not connected: " << qPrintable(unconnected.join(", "));
}

TEST_F(MainWindowTest, everyMenuActionIsConnectedToTheWindow)
{
    ASSERT_NO_FATAL_FAILURE(check_everyMenuActionIsConnectedToTheWindow());
}

void MainWindowTest::check_triggeringLogToFileReachesItsHandler()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructorDriver.stop();

    QAction *logToFile = menuAction(window, kLogToFile);
    ASSERT_NE(logToFile, nullptr);
    ASSERT_FALSE(logToFile->isChecked());
    ASSERT_FALSE(window.write_datalog_to_file_);

    // trigger() toggles the checkable action as a click does; the handler
    // then reads the new state.
    logToFile->trigger();
    EXPECT_TRUE(window.write_datalog_to_file_);
    logToFile->trigger();
    EXPECT_FALSE(window.write_datalog_to_file_);
}

TEST_F(MainWindowTest, triggeringLogToFileReachesItsHandler)
{
    ASSERT_NO_FATAL_FAILURE(check_triggeringLogToFileReachesItsHandler());
}

void MainWindowTest::check_tuneActionsEditTheSelectionThroughTheirOwnHandlers()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
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
    ASSERT_TRUE(
        services.definition_catalogs.RefreshIndex(fastecu::definition::DefinitionFormat::kEcuFlash).has_value());

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
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(window.add_calibration(opened->id));

    auto *fileTree = window.ui_->calibrationFilesTreeWidget;
    ASSERT_EQ(fileTree->topLevelItemCount(), 1);
    fileTree->topLevelItem(0)->setSelected(true);
    window.calibration_files_treewidget_item_selected(fileTree->topLevelItem(0));
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
    window.calibration_data_treewidget_item_selected(gridItem);
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
    const auto grid = [&]
    {
        const bytes::ByteView rom = services.calibrations.Find(opened->id)->Rom();
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
        ASSERT_TRUE(services.calibrations.Find(opened->id)->WriteBytes(kBody, body).has_value());
        select(1, 1, 3, 3);
        action->trigger();
        EXPECT_EQ(grid(), expected) << qPrintable(action->objectName());
    }

    // Set Value edits the original window's selection as it stands when the
    // dialog is accepted, not when the dialog opened.
    ASSERT_TRUE(services.calibrations.Find(opened->id)->WriteBytes(kBody, body).has_value());
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
    ASSERT_TRUE(services.calibrations.Find(opened->id)->WriteBytes(kBody, body).has_value());
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
    ASSERT_NO_FATAL_FAILURE(check_tuneActionsEditTheSelectionThroughTheirOwnHandlers());
}

void MainWindowTest::check_copyFromALargerMapPastesIntoASmallerOneThroughItsScaling()
{
    ModalDriver constructorDriver{QString()};
    constructorDriver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
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
    ASSERT_TRUE(
        services.definition_catalogs.RefreshIndex(fastecu::definition::DefinitionFormat::kEcuFlash).has_value());

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
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(window.add_calibration(opened->id));

    auto *fileTree = window.ui_->calibrationFilesTreeWidget;
    ASSERT_EQ(fileTree->topLevelItemCount(), 1);
    fileTree->topLevelItem(0)->setSelected(true);
    window.calibration_files_treewidget_item_selected(fileTree->topLevelItem(0));
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
                    window.calibration_data_treewidget_item_selected(category->child(child));
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

    const auto destBody = [&]
    {
        const bytes::ByteView rom = services.calibrations.Find(opened->id)->Rom();
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
    ASSERT_NO_FATAL_FAILURE(check_copyFromALargerMapPastesIntoASmallerOneThroughItsScaling());
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

void MainWindowTest::check_typedAssignment(AssignmentScenario scenario)
{
    ModalDriver driver{QString()};
    driver.start();
    TestServices services{config_root_->path()};
    ASSERT_THAT(services.config_status, fastecu::testing::IsOk());
    MainWindow window{services.services()};
    QTemporaryDir files;
    ASSERT_EQ(window.open_calibration_file(writeRom(files, "edit.bin", '\x11')), 0);
    driver.stop();
    const auto id = window.calibrations_.front().id;
    auto *session = services.calibrations.Find(id);
    ASSERT_NE(session, nullptr);
    fastecu::definition::RomDefinition definition;
    definition.scalings.push_back({.name = "Raw",
                                   .from_byte = scenario == AssignmentScenario::kInvalidCurrent ? "1/0" : "x",
                                   .to_byte = "x",
                                   .format = "0.00",
                                   .storage_type = fastecu::definition::StorageType::kInt16,
                                   .endian = "big"});
    fastecu::definition::CalibrationMap model;
    model.name = "Value";
    model.category = "Controls";
    model.type = "1D";
    model.address = 0;
    model.storage_type = fastecu::definition::StorageType::kInt16;
    model.endian = "big";
    model.scaling_name = "Raw";
    definition.maps.push_back(model);
    const bool noOp = scenario == AssignmentScenario::kNoOpResolution || scenario == AssignmentScenario::kNoOpLimit;
    if (noOp)
    {
        definition.scalings[0].from_byte = "x/10";
        definition.scalings[0].to_byte = "x*10";
        definition.scalings[0].fine_increment = "0.01";
        if (scenario == AssignmentScenario::kNoOpLimit)
        {
            definition.scalings[0].maximum = "1";
        }
    }
    *session = fastecu::calibration::CalibrationSession(
        id, {.source = session->Source(),
             .rom = {0, 10},
             .definition = fastecu::calibration::ResolvedDefinition{.definition = definition}});
    window.calibration_files_treewidget_item_selected(window.ui_->calibrationFilesTreeWidget->topLevelItem(0));
    auto *tree = window.ui_->calibrationDataTreeWidget;
    QTreeWidgetItem *item = nullptr;
    for (int row = 0; row < tree->topLevelItemCount(); ++row)
    {
        if (tree->topLevelItem(row)->text(0) == "Controls")
        {
            item = tree->topLevelItem(row)->child(0);
        }
    }
    ASSERT_NE(item, nullptr);
    tree->setCurrentItem(item);
    window.calibration_data_treewidget_item_selected(item);
    ASSERT_EQ(window.ui_->mdiArea->subWindowList().size(), 1);
    auto *subwindow = window.ui_->mdiArea->subWindowList().front();
    window.ui_->mdiArea->setActiveSubWindow(subwindow);
    auto *table = subwindow->findChild<QTableWidget *>();
    ASSERT_NE(table, nullptr);
    table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 0, 0), true);
    std::optional<fastecu::calibration::SessionId> other;
    const bool activeChanged =
        scenario == AssignmentScenario::kActiveMapChanged || scenario == AssignmentScenario::kActiveMapChangedNoOp;
    const bool paste = scenario == AssignmentScenario::kPasteLf || scenario == AssignmentScenario::kPasteCrLf ||
                       scenario == AssignmentScenario::kPasteInteriorEmpty;
    QMdiSubWindow *otherWindow = nullptr;
    if (scenario == AssignmentScenario::kSelectionChanged || activeChanged)
    {
        const auto adopted = services.calibrations.AdoptReadImage({.rom = {0, 40}, .filename = "other.bin"});
        ASSERT_THAT(adopted, fastecu::testing::IsOk());
        other = adopted->id;
        auto *otherSession = services.calibrations.Find(*other);
        ASSERT_NE(otherSession, nullptr);
        *otherSession = fastecu::calibration::CalibrationSession(
            *other, {.source = otherSession->Source(),
                     .rom = {0, 40},
                     .definition = fastecu::calibration::ResolvedDefinition{.definition = definition}});
        ASSERT_TRUE(window.add_calibration(*other));
        if (activeChanged)
        {
            auto *category = window.ui_->calibrationDataTreeWidget->topLevelItem(0);
            for (int row = 0; row < window.ui_->calibrationDataTreeWidget->topLevelItemCount(); ++row)
            {
                auto *candidate = window.ui_->calibrationDataTreeWidget->topLevelItem(row);
                if (candidate->text(0) == "Controls")
                {
                    category = candidate;
                }
            }
            ASSERT_NE(category->child(0), nullptr);
            window.calibration_data_treewidget_item_selected(category->child(0));
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
    }
    bool answered = false;
    QString noticeText;
    QTimer reply;
    reply.setInterval(5);
    QObject::connect(
        &reply, &QTimer::timeout,
        [&]
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
            if (scenario == AssignmentScenario::kCurrentBytes || scenario == AssignmentScenario::kCurrentBytesNoOp ||
                scenario == AssignmentScenario::kActiveMapChangedNoOp)
            {
                EXPECT_THAT(services.calibrations.Find(id)->WriteBytes(0, bytes::Bytes{0, 20}),
                            fastecu::testing::IsOk());
            }
            if (scenario == AssignmentScenario::kSelectionChanged)
            {
                auto *fileTree = window.ui_->calibrationFilesTreeWidget;
                for (int row = 0; row < fileTree->topLevelItemCount(); ++row)
                {
                    fileTree->topLevelItem(row)->setSelected(row == fileTree->topLevelItemCount() - 1);
                }
                window.calibration_files_treewidget_item_selected(
                    fileTree->topLevelItem(fileTree->topLevelItemCount() - 1));
            }
            if (activeChanged)
            {
                window.ui_->mdiArea->setActiveSubWindow(otherWindow);
            }
            if (scenario == AssignmentScenario::kOriginalClosed)
            {
                window.close_calibration();
            }
            dialog->setTextValue(scenario == AssignmentScenario::kAbsolute   ? "-20"
                                 : scenario == AssignmentScenario::kRelative ? "x-20"
                                 : scenario == AssignmentScenario::kInvalidCurrent ||
                                         scenario == AssignmentScenario::kCurrentBytesNoOp ||
                                         scenario == AssignmentScenario::kActiveMapChangedNoOp
                                     ? "20"
                                     : "x+1");
            answered = true;
            dialog->accept();
        });
    reply.start();
    if (paste)
    {
        QApplication::clipboard()->setText(scenario == AssignmentScenario::kPasteLf     ? "20\n"
                                           : scenario == AssignmentScenario::kPasteCrLf ? "20\r\n"
                                                                                        : "20\n\n30");
        answered = true;
        window.paste_value();
    }
    else if (noOp)
    {
        window.inc_dec_value(fastecu::calibration::IncrementStep::kFineUp);
    }
    else
    {
        window.set_value();
    }
    reply.stop();
    ASSERT_TRUE(answered);
    if (scenario == AssignmentScenario::kOriginalClosed)
    {
        EXPECT_EQ(services.calibrations.Find(id), nullptr);
        return;
    }
    session = services.calibrations.Find(id);
    ASSERT_NE(session, nullptr);
    if (noOp)
    {
        EXPECT_EQ(bytes::ReadU16Be(session->Rom()), 10);
        EXPECT_FALSE(session->Dirty());
        if (scenario == AssignmentScenario::kNoOpResolution)
        {
            EXPECT_TRUE(noticeText.contains("storage resolution"));
        }
        else
        {
            EXPECT_TRUE(noticeText.contains("definition limit"));
            EXPECT_FALSE(noticeText.contains("storage resolution"));
        }
        return;
    }
    if (scenario == AssignmentScenario::kPasteInteriorEmpty)
    {
        EXPECT_EQ(bytes::ReadU16Be(session->Rom()), 10);
        EXPECT_FALSE(session->Dirty());
        EXPECT_FALSE(noticeText.isEmpty());
        return;
    }
    if (paste)
    {
        EXPECT_EQ(bytes::ReadU16Be(session->Rom()), 20);
        EXPECT_EQ(table->item(0, 0)->text(), "20.00");
        EXPECT_TRUE(noticeText.isEmpty());
        return;
    }
    const std::uint16_t expected =
        scenario == AssignmentScenario::kAbsolute       ? 65516
        : scenario == AssignmentScenario::kRelative     ? 65526
        : scenario == AssignmentScenario::kCurrentBytes ? 21
        : scenario == AssignmentScenario::kCurrentBytesNoOp || scenario == AssignmentScenario::kActiveMapChangedNoOp
            ? 20
        : scenario == AssignmentScenario::kInvalidCurrent ? 20
                                                          : 11;
    EXPECT_EQ(bytes::ReadU16Be(session->Rom()), expected);
    if (other.has_value())
    {
        EXPECT_EQ(bytes::ReadU16Be(services.calibrations.Find(*other)->Rom()), 40);
        EXPECT_FALSE(services.calibrations.Find(*other)->Dirty());
    }
    if (scenario == AssignmentScenario::kActiveMapChanged)
    {
        EXPECT_EQ(table->item(0, 0)->text(), "11.00");
    }
    if (scenario == AssignmentScenario::kInvalidCurrent)
    {
        EXPECT_EQ(table->item(0, 0)->text(), "NaN");
    }
    if (scenario == AssignmentScenario::kCurrentBytesNoOp || scenario == AssignmentScenario::kActiveMapChangedNoOp)
    {
        EXPECT_EQ(table->item(0, 0)->text(), "20.00");
        EXPECT_TRUE(session->Dirty());
    }
}

TEST_F(MainWindowTest, SignedLiteralAssignsAbsoluteValue)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kAbsolute));
}
TEST_F(MainWindowTest, VariableExpressionEditsRelativeValue)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kRelative));
}
TEST_F(MainWindowTest, AssignmentUsesBytesChangedDuringDialog)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kCurrentBytes));
}
TEST_F(MainWindowTest, AssignmentKeepsOriginalSessionDuringSelectionChange)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kSelectionChanged));
}
TEST_F(MainWindowTest, AssignmentOfClosedOriginalSessionIsInert)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kOriginalClosed));
}
TEST_F(MainWindowTest, AssignmentCanLeaveBrokenDecodeDisplayedAsNan)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kInvalidCurrent));
}

TEST_F(MainWindowTest, SubResolutionIncrementReportsNoChangeAndKeepsClean)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kNoOpResolution));
}
TEST_F(MainWindowTest, ClampedIncrementReportsLimitAndKeepsClean)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kNoOpLimit));
}

TEST_F(MainWindowTest, NoOpAssignmentRefreshesBytesChangedDuringDialog)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kCurrentBytesNoOp));
}

TEST_F(MainWindowTest, AssignmentRefreshesOriginalAfterActiveMapChanges)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kActiveMapChanged));
}
TEST_F(MainWindowTest, NoOpAssignmentRefreshesOriginalAfterActiveMapChanges)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kActiveMapChangedNoOp));
}
TEST_F(MainWindowTest, PasteAcceptsTerminalLf)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kPasteLf));
}
TEST_F(MainWindowTest, PasteAcceptsTerminalCrLf)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kPasteCrLf));
}
TEST_F(MainWindowTest, PasteRejectsInteriorEmptyCellAtomically)
{
    ASSERT_NO_FATAL_FAILURE(check_typedAssignment(AssignmentScenario::kPasteInteriorEmpty));
}
