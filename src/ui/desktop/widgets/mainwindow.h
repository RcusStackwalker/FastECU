#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Exit application with this code to restart it instead of quitting:
// qApp->exit(kRestartCode)
inline constexpr int kRestartCode = 1000;

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
#include <QSplashScreen>
#include <QAtomicInteger>
#include <QFontDatabase>
#include <QRemoteObjectReplica>

#include <QFuture>
#include <QDebug>
#include <QThread>
#include <QMutex>

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
#include "src/ui/desktop/channels/remote_peer.h"

// Flash modules

// OBD

#include "src/platform/desktop/common/logging/runtime/logging_engine.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#include "src/platform/desktop/common/logging/logging_value_adapter.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/connection/adapter_connection.h"
#include "src/ui/desktop/connection/connection_ports.h"

#include <functional>

extern void logError(const QString& message, bool timestamp, bool linefeed);
extern void logWarning(const QString& message, bool timestamp, bool linefeed);
extern void logInfo(const QString& message, bool timestamp, bool linefeed);
extern void logDebug(const QString& message, bool timestamp, bool linefeed);

namespace fastecu::ui
{
class CalibrationOperationCoordinator;
class ConnectionCoordinator;
class QtCalibrationInteraction;
class QtIdentifyLauncher;
} // namespace fastecu::ui

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
    MainWindow(MainWindowServices services, const QString& peerAddress = "", QWidget *parent = nullptr);
    ~MainWindow();

    void delay(int n);

  private:
    MainWindowServices services_;

    QString software_name_;
    QString software_title_;
    QString software_version_;

    QMutex restart_question_active_;

    QString peer_address_;
    QSplashScreen *net_splash_;
    fastecu::ui::RemotePeer *remote_peer_ = nullptr;
    static const QColor kRedLightOff;
    static const QColor kRedLightOn;
    static const QColor kYellowLightOff;
    static const QColor kYellowLightOn;
    static const QColor kGreenLightOff;
    static const QColor kGreenLightOn;

    bool logging_state_ = false;
    bool log_params_request_started_ = false;
    bool ecu_init_complete_ = false;

    uint16_t receive_timeout_ = 500;
    uint16_t serial_read_timeout_ = 2000;
    uint16_t serial_read_extra_short_timeout_ = 50;
    uint16_t serial_read_short_timeout_ = 200;
    uint16_t serial_read_medium_timeout_ = 500;
    uint16_t serial_read_long_timeout_ = 800;
    uint16_t serial_read_extra_long_timeout_ = 3000;

    int map_cell_width_selectable_ = 240;
    int map_cell_width1_d_ = 96;
    int map_cell_width_ = 54;
    int map_cell_height_ = 26;
    int cell_font_size_ = static_cast<int>(map_cell_height_ / 2.25);

    int x_size_ = 0;
    int y_size_ = 0;

    int connection_time_out_delay_ = 5;
    int connection_time_out_delay_count_ = 50;

    // Declared interaction first, so the coordinator borrowing it is
    // destroyed first.
    std::unique_ptr<fastecu::ui::QtCalibrationInteraction> calibration_interaction_;
    std::unique_ptr<fastecu::ui::CalibrationOperationCoordinator> calibration_operations_;
    fastecu::ui::DefinitionAuthoringDialog *definition_authoring_dialog_ = nullptr;
    fastecu::logging::LoggerModel *logger_model_;
    fastecu::desktop::logging::DesktopLoggerValues logger_values_;
    void loadLoggerDefinition();
    void loadLoggerSelection();
    void saveLoggerSelection();
    void writeLoggerCsvRecord(bool header);
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
    // QTimer *serial_poll_timer;
    uint16_t serial_poll_timer_timeout_ = 500;
    QString serial_port_baudrate_ = "4800";
    QString default_serial_port_baudrate_ = "4800";
    QString serial_port_linux_ = "/dev/ttyUSB0";
    QString serial_port_windows_ = "COM67";
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

    int ecu_protocols_list_length_ = 6;
    QString current_car_model_ = "";

    // QStringList flash_methods;
    QStringList flash_transports_;
    QStringList log_transports_;

    //        "Mercedes",     "CR3 EDC16C31",     "K-Line",           "K-Line",           "iso14230", "Mercedes Benz
    //        320CDI",

    enum RomInfoEnum
    {
        kXmlId,
        kInternalIdAddress,
        kInternalIdString,
        kEcuId,
        kMake,
        kMarket,
        kModel,
        kSubModel,
        kTransmission,
        kYear,
        kFlashMethod,
        kMemModel,
        kChecksumModule,
        kRomBase,
        kFileSize,
        kDefFile,
    };

    QString ecuid_ = "";
    QString protocol_ = "";
    QString log_protocol_ = "";

    QTimer *vbatt_timer_;
    uint16_t vbatt_timer_timeout_ = 1000;
    uint16_t vbatt_timer_comms_timeout_ = 5000;

    // QTimer *ssm_init_poll_timer;
    uint16_t ssm_init_poll_timer_timeout_ = 250;

    fastecu::desktop::logging::LoggingEngine *logging_engine_ = nullptr;
    std::optional<fastecu::desktop::logging::DesktopLoggingSnapshot> active_logging_snapshot_;
    QString active_log_value_protocol_filter_;

    LogBox *log_boxes_;

    QRadioButton *ecu_radio_button_;
    QRadioButton *tcu_radio_button_;

    QTreeWidget tree_widget_;
    CalibrationTreeWidget *calibration_tree_widget_ = new CalibrationTreeWidget();

    QLabel *status_bar_connection_label_ = new QLabel("");
    QLabel *status_bar_ecu_label_ = new QLabel("");

    QMenu *main_window_menu_{};

    QPushButton *refresh_serial_port_list_;
    QComboBox *serial_port_list_;
    QComboBox *flash_transport_list_;
    QComboBox *log_transport_list_;

    QFile datalog_file_;
    QFile syslog_file_;
    QTextStream datalog_file_outstream_;
    QTextStream syslog_file_outstream_;
    bool write_datalog_to_file_ = false;
    bool write_syslog_to_file_ = false;
    bool datalog_file_open_ = false;
    bool syslog_file_open_ = false;
    std::unique_ptr<QElapsedTimer> log_file_timer_;

    QDialog *settings_dialog_{};
    QListWidget *contents_widget_{};
    QStackedWidget *pages_widget_{};

    QSize toolbar_item_size_ = QSize(24, 24);

    fastecu::ui::LogChannel *log_channel_ = nullptr;

    bool eventFilter(QObject *target, QEvent *event);

    // fileactions.c
    bool openCalibrationFile(QString filename);
    void promptForMissingDefinition(fastecu::calibration::SessionId id);
    void saveCalibrationFile();
    void saveCalibrationFileAs();
    void setMapSelection(fastecu::calibration::SessionId id, int mapIndex, const QString& item);
    void setMapSwitch(fastecu::calibration::SessionId id, int mapIndex, int state);
    QStringList parseStringlistFromExpressionString(QString expression, QString x);
    float calculateValueFromExpression(QStringList expression);

    // log_operations
    void parseLogValueList(QByteArray received, const QString& protocolArg);
    void logToFile();

    void setupLoggingEngine();
    void restoreLoggingUiState();

    // logvalues.c
    void changeLogValues(int tabIndex, const QString& protocolArg);

    // mainwindow.c
    // Connect signals for any flash class and execute ::run() method
    template <typename FlashClass> FlashClass *connectSignalsAndRunModule(FlashClass *object);
    void setComboBoxItemEnabled(QComboBox *comboBox, int index, bool enabled);
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
    // QString check_kernel(QString flash_method);
    QTextEdit *iterateWidgetChild(QObjectList children);
    bool writeSyslog(QString msg);

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
    void loggerDefinitionManager();
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
    void handleLoggingValuesUpdated(const QVector<fastecu::logging::LogSample>& samples);
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
    int startEcuOperations(const QString& cmdType);
    void closeCalibration();
    void closeCalibrationMap(QObject *obj);
    void changeGaugeValues();
    void changeDigitalValues();
    void changeSwitchValues();
    void updateLogboxes(const QString& protocolArg);
    void updateLogboxValues(const QString& protocolArg);
    void addNewEcuDefinitionFile();
    void removeEcuDefinitionFile();
    void addNewLoggerDefinitionFile();
    void removeLoggerDefinitionFile();
    QString parseMessageToHex(const QByteArray& received);
    void setStatusBarLabel(bool serialConnectionState, bool ecuConnectionState, const QString& romId);
    void customMenuRequested(QPoint pos);
    void selectableComboboxItemChanged(const QString& item);
    void checkboxStateChanged(int state);
    void closeApp();
    // Logger
    // void logger(int log_level, QString message, bool timestamp, bool linefeed);
    // void logger(QString message, bool timestamp, bool linefeed);
    // void sendMsgToLogWindow(QWidget* parent, QString msg);
    void sendMessageToLogWindow(const QString& msg);
    void networkStateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);

    // logvalues.c
    void changeLogGaugeValue(int index);
    void changeLogDigitalValue(int index);
    void changeLogSwitchValue(int index);

    void updateVbatt();

  signals:
    void checkSerialPort();
    void sendSerialData(QByteArray output);
    void logE(QString message, bool timestamp, bool linefeed);
    void logW(QString message, bool timestamp, bool linefeed);
    void logI(QString message, bool timestamp, bool linefeed);
    void logD(QString message, bool timestamp, bool linefeed);
    // void syslog(int logType, bool write_syslog_to_file, QString message, bool timestamp, bool linefeed);
    void syslog(QString message, bool timestamp, bool linefeed);
    void enableLogWriteToFile(bool enable);

  private:
    std::unique_ptr<Ui::MainWindow> ui_;
};
