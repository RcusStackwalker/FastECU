#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QMainWindow>
#include <QTreeWidget>
#include <QLabel>
#include <QGroupBox>
#include <QHeaderView>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QRect>
#include <QTimer>
#include <QTableWidget>
#include <QInputDialog>
#include <QLineEdit>
#include <QClipboard>
#include <QTime>
#include <QProgressDialog>
#include <QListWidget>
#include <QCheckBox>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QAtomicInteger>
#include <QFontDatabase>

#include <QFuture>
#include <QDebug>
#include <QThread>

#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/ui/desktop/calibration/calibration_view_state.h"
#include "src/ui/desktop/widgets/calibration_maps.h"
#include "src/ui/desktop/widgets/calibration_treewidget.h"
#include "src/ui/desktop/widgets/protocol_select.h"
#include "src/ui/desktop/widgets/vehicle_select.h"
#include "src/ui/desktop/biu/biu_operations_subaru.h"
#include "src/ui/desktop/widgets/dataterminal.h"
#include "src/ui/desktop/widgets/get_key_operations_subaru.h"
#include "src/backend/calibration/map_edit.h"
#include "src/backend/config/config_session.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/definition/definition_catalog_session.h"
#include "src/ui/desktop/definition/dialog/definition_authoring_dialog.h"
#include "src/platform/desktop/common/ports/event_sink/qt_event_sink.h"
#include "src/ui/desktop/widgets/logbox.h"
#include "src/ui/desktop/main_window_services.h"
#include "src/ui/desktop/widgets/settings.h"
#include "src/ui/desktop/widgets/dtc_operations.h"
#include "src/ui/desktop/hexedit/hexedit.h"
#include "src/ui/desktop/channels/log_channel.h"

#include "src/platform/desktop/common/logging/runtime/logging_engine.h"
#include "src/platform/desktop/common/logging/logging_value_adapter.h"
#include "src/ui/desktop/widgets/desktop_logging_run.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/connection/adapter_connection.h"
#include "src/ui/desktop/connection/connection_ports.h"

#include <functional>

namespace fastecu::ui
{
class CalibrationOperationCoordinator;
class ConnectionCoordinator;
class QtCalibrationInteraction;
class QtIdentifyLauncher;
} // namespace fastecu::ui

namespace fastecu::flash
{
struct FlashOperationInput;
struct FlashOperationOutcome;
} // namespace fastecu::flash

QT_BEGIN_NAMESPACE
namespace Ui
{
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

    friend class MainWindowTest;

  public:
    explicit MainWindow(MainWindowServices services, QWidget *parent = nullptr);
    ~MainWindow();

    void delay(int n);

  private:
    MainWindowServices services_;
    std::unique_ptr<fastecu::desktop::logging::DesktopLoggingRun> logging_run_;

    QString software_title_;
    QString software_version_;

    bool logging_state_ = false;
    bool ecu_init_complete_ = false;

    // Declared interaction first, so the coordinator borrowing it is
    // destroyed first.
    std::unique_ptr<fastecu::ui::QtCalibrationInteraction> calibration_interaction_;
    std::unique_ptr<fastecu::ui::CalibrationOperationCoordinator> calibration_operations_;
    fastecu::ui::DefinitionAuthoringDialog *definition_authoring_dialog_ = nullptr;
    fastecu::logging::LoggerModel *logger_model_;
    void loadLoggerDefinition();
    // Persists the resolved bundled definition handle into the settings.
    void useBundledLoggerDefinition(const std::string& handle);
    void loadLoggerSelection();
    // Applies and persists `selection` for the current ECU, noticing a failed save.
    void savePendingLoggingSelection(fastecu::logging::LoggerSelection selection);
    void reportLoggingCsvError(const QString& message);
    fastecu::config::ConfigSession *config_session_ = nullptr;
    std::optional<fastecu::Error> last_settings_save_error_;
    // Desktop owns only identity and presentation; the workspace owns ROM data.
    struct OpenCalibration
    {
        fastecu::calibration::SessionId id{};
        fastecu::ui::CalibrationViewState view;
    };
    std::vector<OpenCalibration> calibrations_;
    fastecu::calibration::CalibrationWorkspace *calibration_workspace_ = nullptr;

    fastecu::calibration::CalibrationSession *calibration(fastecu::calibration::SessionId id);
    std::optional<fastecu::calibration::SessionId> sessionOf(const QTreeWidgetItem *filesItem) const;
    fastecu::calibration::CalibrationSession *selectedCalibration();
    OpenCalibration *openCalibration(fastecu::calibration::SessionId id);
    OpenCalibration *selectedOpenCalibration();
    void setCategoryExpanded(QTreeWidgetItem *item, bool expanded);
    QTreeWidgetItem *filesTreeItem(fastecu::calibration::SessionId id) const;
    bool addCalibration(fastecu::calibration::SessionId id);

    fastecu::desktop::connection::AdapterConnection *connection_ = nullptr;
    QString serial_port_baudrate_ = "4800";
    QString default_serial_port_baudrate_ = "4800";
    QString serial_port_;
    QString previous_serial_port_;
    QString serial_port_prefix_;
    QStringList serial_ports_;

    // The port chosen in the toolbar, or empty when there is none. Inline so
    // tests reaching it through `#define private public` link on MSVC too.
    QString selectedSerialPort() const
    {
        return serial_ports_.value(serial_port_list_->currentIndex());
    }

    // open_serial_port's bookkeeping once a port opened: forget the ECU when
    // the port changed, and remember the port for the next launch.
    void rememberOpenedPort(const QString& port, const QString& openedPort);

    QString ecuid_ = "";
    QString protocol_ = "";

    QTimer *vbatt_timer_;
    uint16_t vbatt_timer_timeout_ = 1000;

    LogBox *log_boxes_;

    QRadioButton *ecu_radio_button_;
    QRadioButton *tcu_radio_button_;

    CalibrationTreeWidget *calibration_tree_widget_ = new CalibrationTreeWidget();

    QLabel *status_bar_connection_label_ = new QLabel("");
    QLabel *logging_pending_label_ = new QLabel();
    QLabel *status_bar_ecu_label_ = new QLabel("");

    QPushButton *refresh_serial_port_list_;
    QComboBox *serial_port_list_;
    QComboBox *flash_transport_list_;
    QComboBox *log_transport_list_;

    QSize toolbar_item_size_ = QSize(24, 24);

    fastecu::ui::LogChannel *log_channel_ = nullptr;

    // fileactions.c
    bool openCalibrationFile(QString filename);
    void promptForMissingDefinition(fastecu::calibration::SessionId id);
    void saveCalibrationFile();
    void saveCalibrationFileAs();
    void setMapSelection(fastecu::calibration::SessionId id, int mapIndex, const QString& item);
    void setMapSwitch(fastecu::calibration::SessionId id, int mapIndex, int state);

    // log_operations
    void parseLogValueList(QByteArray received, const QString& protocolArg);

    void setupLoggingEngine();
    void restoreLoggingUiState();
    struct MeasurementLabel
    {
        QString name;
        QString unit;
        bool available = false;
    };
    MeasurementLabel measurementLabel(const fastecu::desktop::logging::DesktopLoggingRunView& view,
                                      const std::string& key, fastecu::logging::LoggingMeasurementKind kind,
                                      const std::string& id) const;

    // Constructor steps, in order.
    void logPlatform();
    void createCoordinators();
    void logSelectedVehicle();
    void restoreWindowGeometry();
    void warnIfNoDefinitionFiles();
    void connectWidgetSignals();
    void setupStatusBar();
    void setupToolbar();
    void addSerialPortList();

    void runFlashOperation(const QString& cmdType, const std::string& kernelDir);
    fastecu::flash::FlashOperationOutcome runFlashController(const fastecu::flash::FlashOperationInput& input);
    void adoptReadImage(const fastecu::flash::FlashOperationOutcome& outcome, const QString& kernelPath,
                        const QString& kernelAddress);
    void toggleMapWindow(QTreeWidgetItem *item, OpenCalibration& open, const QString& windowPrefix);
    void openMapWindow(QTreeWidgetItem *item, OpenCalibration& open, fastecu::calibration::SessionId session,
                       int mapIndex);

    // logvalues.c
    void changeLogValues(int tabIndex, const QString& protocolArg);

    // mainwindow.c
    void setFlashArrowState();
    void updateProtocolInfo(const QString& flashMethod);
    // The session's selected vehicle; always valid once constructed.
    const fastecu::config::VehicleSpec& selectedVehicle() const;
    // Saves the session's settings, logging a failure.
    void saveSettings();
    // Emits the LOG_* signal for `level`, with timestamp and linefeed.
    void emitLogLine(fastecu::LogLevel level, const QString& message);
    // Apply a finished dialog's tentative choice: only an accepted one
    // reaches the session. Both then run the matching *_finished slot.
    void applyVehicleChoice(int result, std::optional<std::size_t> row);
    void applyProtocolChoice(int result, std::optional<std::string> protocolName);
    QStringList createFlashTransportsList();
    QStringList createLogTransportsList();

    // menuactions.c
    void connectMenuActions();
    void applyStandardShortcuts();
    void showAboutDialog();
    void incDecValue(fastecu::calibration::IncrementStep step);
    void setValue();
    void interpolateValue(fastecu::calibration::InterpolationMode mode);
    void copyValue();
    void pasteValue();
    // Opens the port and, for Subaru, identifies the ECU on a worker thread.
    // on_done(false) means the port did not open or identification was
    // stopped; on_done(true) means the port opened, whether or not the ECU
    // answered (unchanged from the synchronous code).
    //
    // Every entry point that touches the serial facade (disconnect, flash, DTC,
    // BIU, terminal, a port or transport change) first calls
    // connection_coordinator_->cancel(), which can run on_done(false)
    // synchronously. on_done must therefore not start a connection or otherwise
    // touch the facade synchronously; defer any such work to the event loop.
    // See ConnectionCoordinator::begin.
    void connectToEcu(std::function<void(bool)> onDone = {});
    void continueStartLogging();

    // What a connection attempt shows and changes in this window. Methods are
    // defined in menu_actions.cpp.
    class ConnectionPresentation final : public fastecu::ui::IConnectionPresentation
    {
      public:
        explicit ConnectionPresentation(MainWindow& window) : window_(window)
        {
        }

        void setControlsLocked(bool locked) override;
        void setPortSelectorEnabled(bool enabled) override;
        void identified(const fastecu::ui::IdentifyOutcome& outcome) override;
        void identificationFailed(const fastecu::ui::IdentifyOutcome& outcome) override;

      private:
        MainWindow& window_;
    };

    // Declared in this order so the coordinator, which borrows the other two,
    // is destroyed first.
    ConnectionPresentation connection_presentation_{*this};
    std::unique_ptr<fastecu::ui::QtIdentifyLauncher> identify_launcher_;
    std::unique_ptr<fastecu::ui::ConnectionCoordinator> connection_coordinator_;
    void disconnectFromEcu();
    void ecuDefinitionManager();
    void winolsCsvToRomraiderXml();
    void setRealtimeState(bool state);
    void toggleRealtime();
    void toggleLogToFile();
    void setMaptablewidgetItems();
    void showPreferencesWindow();

    void showDtcWindow();
    void showHexEditor();
    void showSubaruBiuWindow();
    void showTerminalWindow();
    void showSubaruGetKeyWindow();

  protected:
    void closeEvent(QCloseEvent *event);
    bool event(QEvent *event);
    void resizeEvent(QResizeEvent *event);

  private slots:
    // External logger slot for string messages
    void externalLogger(const QString& message);
    // External progress bar slot
    void externalLoggerSetProgressbarValue(int value);

    // calibrationtreewidget.c
    void calibrationFilesTreewidgetItemSelected(QTreeWidgetItem *item);
    void calibrationDataTreewidgetItemSelected(QTreeWidgetItem *item);
    void calibrationDataTreewidgetItemExpanded(QTreeWidgetItem *item);
    void calibrationDataTreewidgetItemCollapsed(QTreeWidgetItem *item);

    // log_operations.c
    void handleLoggingSessionEnded(fastecu::desktop::logging::SessionEndReason reason, const QString& message);

    // mainwindow.c
    void selectProtocol();
    void selectProtocolFinished(int result);
    void selectVehicle();
    void selectVehicleFinished(int result);
    void logTransportChanged();
    void flashTransportChanged();
    void checkSerialPorts();
    void openSerialPort();
    void startEcuOperations(const QString& cmdType);
    void closeCalibration();
    void closeCalibrationMap(QObject *obj);
    void changeGaugeValues();
    void changeDigitalValues();
    void changeSwitchValues();
    void updateLogboxes(const QString& protocolArg);
    void updateLogboxValues(const QString& protocolArg);
    void updateLoggingPendingState();
    void addNewEcuDefinitionFile();
    void removeEcuDefinitionFile();
    void setStatusBarLabel(bool serialConnectionState, bool ecuConnectionState, const QString& romId);
    void customMenuRequested(QPoint pos);
    void selectableComboboxItemChanged(const QString& item);
    void checkboxStateChanged(int state);
    void closeApp();
    // Logger
    void sendMessageToLogWindow(const QString& msg);

    // logvalues.c
    void changeLogGaugeValue(int index);
    void changeLogDigitalValue(int index);
    void changeLogSwitchValue(int index);

    void updateVbatt();

  signals:
    void logE(QString message, bool timestamp, bool linefeed);
    void logW(QString message, bool timestamp, bool linefeed);
    void logI(QString message, bool timestamp, bool linefeed);
    void logD(QString message, bool timestamp, bool linefeed);
    void enableLogWriteToFile(bool enable);

  private:
    std::unique_ptr<Ui::MainWindow> ui_;
};
