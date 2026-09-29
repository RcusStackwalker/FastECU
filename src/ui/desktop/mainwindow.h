#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Exit application with this code to restart it instead of quitting:
// qApp->exit(RESTART_CODE)
#define RESTART_CODE 1000

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
#include <QSignalMapper>
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
#include "src/ui/desktop/calibration_maps.h"
#include "src/ui/desktop/calibration_treewidget.h"
#include "src/ui/desktop/protocol_select.h"
#include "src/ui/desktop/vehicle_select.h"
#include "src/ui/desktop/definition_file_convert.h"
#include "src/ui/desktop/biu/biu_operations_subaru.h"
#include "src/ui/desktop/dataterminal.h"
#include "src/ui/desktop/get_key_operations_subaru.h"
#include "src/backend/calibration/map_edit.h"
#include "src/backend/config/config_session.h"
#include "src/platform/desktop/common/definition/definition_catalog_session.h"
#include "src/ui/desktop/checksum/checksum_correction_command.h"
#include "src/ui/desktop/definition/definition_authoring_dialog.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/ui/desktop/logbox.h"
#include "src/ui/desktop/main_window_services.h"
#include "src/ui/desktop/settings.h"
#include "src/ui/desktop/dtc_operations.h"
#include "src/ui/desktop/hexedit/hexedit.h"
#include "src/ui/desktop/channels/log_channel.h"
#include "src/ui/desktop/channels/remote_peer.h"

// Flash modules

// OBD

#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#include "src/platform/desktop/common/logging/logging_value_adapter.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/connection/adapter_connection.h"
#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"
#include "src/platform/desktop/common/diagnostics/ssm_identify_worker.h"

#include <functional>

extern void log_error(const QString& message, bool timestamp, bool linefeed);
extern void log_warning(const QString& message, bool timestamp, bool linefeed);
extern void log_info(const QString& message, bool timestamp, bool linefeed);
extern void log_debug(const QString& message, bool timestamp, bool linefeed);

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

    enum
    {
        _LOG_E = 0, // error
        _LOG_W,     // warning
        _LOG_I,     // info
        _LOG_D,     // debug
    };

    QString software_name;
    QString software_title;
    QString software_version;

    std::unique_ptr<QSplashScreen> startUpSplash;
    QLabel *startUpSplashLabel;
    QProgressBar *startUpSplashProgressBar;
    QMutex restartQuestionActive;

    QString peerAddress;
    QSplashScreen *netSplash;
    fastecu::ui::RemotePeer *remote_peer = nullptr;
    static const QColor RED_LIGHT_OFF;
    static const QColor RED_LIGHT_ON;
    static const QColor YELLOW_LIGHT_OFF;
    static const QColor YELLOW_LIGHT_ON;
    static const QColor GREEN_LIGHT_OFF;
    static const QColor GREEN_LIGHT_ON;

    bool logging_state = false;
    bool log_params_request_started = false;
    bool ecu_init_complete = false;

    uint16_t receive_timeout = 500;
    uint16_t serial_read_timeout = 2000;
    uint16_t serial_read_extra_short_timeout = 50;
    uint16_t serial_read_short_timeout = 200;
    uint16_t serial_read_medium_timeout = 500;
    uint16_t serial_read_long_timeout = 800;
    uint16_t serial_read_extra_long_timeout = 3000;

    int mapCellWidthSelectable = 240;
    int mapCellWidth1D = 96;
    int mapCellWidth = 54;
    int mapCellHeight = 26;
    int cellFontSize = mapCellHeight / 2.25;

    int xSize = 0;
    int ySize = 0;

    int connectionTimeOutDelay = 5;
    int connectionTimeOutDelayCount = 50;

    fastecu::ui::ChecksumCorrectionCommand m_checksumCorrectionCommand;
    fastecu::ui::DefinitionAuthoringDialog *definitionAuthoringDialog = nullptr;
    fastecu::logging::LoggerModel *loggerModel;
    fastecu::desktop::logging::DesktopLoggerValues loggerValues;
    void load_logger_definition();
    void load_logger_selection();
    void save_logger_selection();
    void write_logger_csv_cells(bool header);
    fastecu::config::ConfigSession *configSession = nullptr;
    std::optional<fastecu::Error> last_settings_save_error;
    // Desktop owns only identity and presentation; the workspace owns ROM data.
    struct OpenCalibration
    {
        fastecu::calibration::SessionId id;
        fastecu::ui::CalibrationViewState view;
    };
    std::vector<OpenCalibration> calibrations_;
    fastecu::calibration::CalibrationWorkspace *calibrationWorkspace = nullptr;

    fastecu::calibration::CalibrationSession *calibration(fastecu::calibration::SessionId id);
    std::optional<fastecu::calibration::SessionId> session_of(const QTreeWidgetItem *files_item) const;
    fastecu::calibration::CalibrationSession *selected_calibration();
    OpenCalibration *open_calibration(fastecu::calibration::SessionId id);
    OpenCalibration *selected_open_calibration();
    void set_category_expanded(QTreeWidgetItem *item, bool expanded);
    void refresh_write_metadata(fastecu::calibration::CalibrationSession& session, const QString& kernel_dir);
    QTreeWidgetItem *files_tree_item(fastecu::calibration::SessionId id) const;
    bool add_calibration(fastecu::calibration::SessionId id);

    fastecu::desktop::connection::AdapterConnection *connection = nullptr;
    // QTimer *serial_poll_timer;
    uint16_t serial_poll_timer_timeout = 500;
    QString serial_port_baudrate = "4800";
    QString default_serial_port_baudrate = "4800";
    QString serial_port_linux = "/dev/ttyUSB0";
    QString serial_port_windows = "COM67";
    QString serial_port;
    QString previous_serial_port;
    QString serial_port_prefix;
    QStringList serial_ports;

    // The port chosen in the toolbar, or empty when there is none. Inline so
    // tests reaching it through `#define private public` link on MSVC too.
    QString selected_serial_port() const
    {
        return serial_ports.value(serial_port_list->currentIndex());
    }

    // open_serial_port's bookkeeping once a port opened: forget the ECU when
    // the port changed, and remember the port for the next launch.
    void remember_opened_port(const QString& port, const QString& opened_port);

    int ecu_protocols_list_length = 6;
    QString current_car_model = "";

    // QStringList flash_methods;
    QStringList flash_transports;
    QStringList log_transports;

    //        "Mercedes",     "CR3 EDC16C31",     "K-Line",           "K-Line",           "iso14230", "Mercedes Benz
    //        320CDI",

    enum RomInfoEnum
    {
        XmlId,
        InternalIdAddress,
        InternalIdString,
        EcuId,
        Make,
        Market,
        Model,
        SubModel,
        Transmission,
        Year,
        FlashMethod,
        MemModel,
        ChecksumModule,
        RomBase,
        FileSize,
        DefFile,
    };

    QString ecuid = "";
    QString protocol = "";
    QString log_protocol = "";

    QTimer *vbatt_timer;
    uint16_t vbatt_timer_timeout = 1000;
    uint16_t vbatt_timer_comms_timeout = 5000;

    // QTimer *ssm_init_poll_timer;
    uint16_t ssm_init_poll_timer_timeout = 250;

    fastecu::desktop::logging::LoggingEngine *loggingEngine = nullptr;
    std::optional<fastecu::desktop::logging::DesktopLoggingSnapshot> activeLoggingSnapshot;
    QString activeLogValueProtocolFilter;

    LogBox *logBoxes;

    QRadioButton *ecu_radio_button;
    QRadioButton *tcu_radio_button;

    QTreeWidget treeWidget;
    CalibrationTreeWidget *calibrationTreeWidget = new CalibrationTreeWidget();

    QLabel *status_bar_connection_label = new QLabel("");
    QLabel *status_bar_ecu_label = new QLabel("");

    QMenu *mainWindowMenu{};

    QPushButton *refresh_serial_port_list;
    QComboBox *serial_port_list;
    QComboBox *flash_transport_list;
    QComboBox *log_transport_list;

    QFile datalog_file;
    QFile syslog_file;
    QTextStream datalog_file_outstream;
    QTextStream syslog_file_outstream;
    bool write_datalog_to_file = false;
    bool write_syslog_to_file = false;
    bool datalog_file_open = false;
    bool syslog_file_open = false;
    std::unique_ptr<QElapsedTimer> log_file_timer;

    QDialog *settings_dialog{};
    QListWidget *contents_widget{};
    QStackedWidget *pages_widget{};

    QSize toolbar_item_size = QSize(24, 24);

    fastecu::ui::LogChannel *log_channel = nullptr;

    bool eventFilter(QObject *target, QEvent *event);

    // fileactions.c
    bool open_calibration_file(QString filename);
    void prompt_for_missing_definition(fastecu::calibration::SessionId id);
    void save_calibration_file();
    void save_calibration_file_as();
    void runChecksumCorrection(const fastecu::calibration::CalibrationSession& session, bytes::Bytes& image);
    void set_map_selection(fastecu::calibration::SessionId id, int map_index, const QString& item);
    void set_map_switch(fastecu::calibration::SessionId id, int map_index, int state);
    QStringList parse_stringlist_from_expression_string(QString expression, QString x);
    float calculate_value_from_expression(QStringList expression);

    // log_operations
    void parse_log_value_list(QByteArray received, const QString& protocol_arg);
    void log_to_file();

    void setupLoggingEngine();
    void restoreLoggingUiState();

    // logvalues.c
    void change_log_values(int tabIndex, const QString& protocol_arg);

    // mainwindow.c
    // Connect signals for any flash class and execute ::run() method
    template <typename FLASH_CLASS> FLASH_CLASS *connect_signals_and_run_module(FLASH_CLASS *object);
    void SetComboBoxItemEnabled(QComboBox *comboBox, int index, bool enabled);
    void set_flash_arrow_state();
    void update_protocol_info(const QString& flash_method);
    // The session's selected vehicle; always valid once constructed.
    const fastecu::config::ResolvedCarModel& selected_vehicle() const;
    // Saves the session's settings, logging a failure.
    void save_settings();
    // Apply a finished dialog's tentative choice: only an accepted one
    // reaches the session. Both then run the matching *_finished slot.
    void apply_vehicle_choice(int result, std::optional<std::size_t> row);
    void apply_protocol_choice(int result, std::optional<std::string> protocol_name);
    QStringList create_flash_transports_list();
    QStringList create_log_transports_list();
    // QString check_kernel(QString flash_method);
    void setSplashScreenProgress(const QString& text, int incValue);
    QTextEdit *iterateWidgetChild(QObjectList children);
    bool write_syslog(QString msg);

    // menuactions.c
    void inc_dec_value(fastecu::calibration::IncrementStep step);
    void set_value();
    void interpolate_value(fastecu::calibration::InterpolationMode mode);
    void copy_value();
    void paste_value();
    // Opens the port and, for Subaru, identifies the ECU on a worker thread.
    // on_done(false) means the port did not open or identification was
    // stopped; on_done(true) means the port opened, whether or not the ECU
    // answered (unchanged from the synchronous code).
    //
    // on_done(false) can run synchronously inside stop_identification(),
    // before that caller (disconnect, flash, DTC, BIU, terminal, a port or
    // transport change) goes on to use the facade. on_done must therefore not
    // start a connection or otherwise touch the facade synchronously; defer
    // any such work to the event loop.
    void connect_to_ecu(std::function<void(bool)> on_done = {});
    void continue_start_logging();
    void finish_identification(const fastecu::diagnostics::SsmIdentifyWorkerResult& result);
    // Cancels and joins a running identification, restores the controls
    // (including the port selector connect_to_ecu locked), and tells a waiting
    // caller the connect did not complete. That caller's on_done(false) runs
    // here, synchronously, before stop_identification returns (see
    // connect_to_ecu for the contract this places on on_done).
    void stop_identification();
    void set_identification_in_progress(bool in_progress);

    // Declared link first, so the worker (which uses it) is destroyed first.
    std::unique_ptr<fastecu::diagnostics::SerialDiagnosticLink> identify_link_;
    std::unique_ptr<fastecu::diagnostics::SsmIdentifyWorker> identify_worker_;
    std::function<void(bool)> connect_done_;
    // Bumped by every start and stop, so a completion queued by a worker that
    // was stopped is recognised as stale and dropped.
    quint64 identify_generation_ = 0;
    void disconnect_from_ecu();
    void ecu_definition_manager();
    void logger_definition_manager();
    void winols_csv_to_romraider_xml();
    void set_realtime_state(bool state);
    void toggle_realtime();
    void toggle_log_to_file();
    void set_maptablewidget_items();
    void show_preferences_window();

    void show_dtc_window();
    void show_hex_editor();
    void show_subaru_biu_window();
    void show_terminal_window();
    void show_subaru_get_key_window();

  protected:
    void closeEvent(QCloseEvent *event);
    bool event(QEvent *event);
    void resizeEvent(QResizeEvent *event);

  private slots:
    // External logger slot for string messages
    void external_logger(const QString& message);
    // External progress bar slot
    void external_logger_set_progressbar_value(int value);

    // calibrationtreewidget.c
    void calibration_files_treewidget_item_selected(QTreeWidgetItem *item);
    void calibration_data_treewidget_item_selected(QTreeWidgetItem *item);
    void calibration_data_treewidget_item_expanded(QTreeWidgetItem *item);
    void calibration_data_treewidget_item_collapsed(QTreeWidgetItem *item);

    // log_operations.c
    void handleLoggingValuesUpdated(const QVector<fastecu::logging::LogSample>& samples);
    void handleLoggingSessionEnded(fastecu::desktop::logging::SessionEndReason reason, const QString& message);

    // menu_actions.c
    void menu_action_triggered(const QString& action);

    // mainwindow.c
    void select_protocol();
    void select_protocol_finished(int result);
    void select_vehicle();
    void select_vehicle_finished(int result);
    void log_transport_changed();
    void flash_transport_changed();
    void check_serial_ports();
    void open_serial_port();
    int start_ecu_operations(const QString& cmd_type);
    void close_calibration();
    void close_calibration_map(QObject *obj);
    void change_gauge_values();
    void change_digital_values();
    void change_switch_values();
    void update_logboxes(const QString& protocol_arg);
    void update_logbox_values(const QString& protocol_arg);
    void add_new_ecu_definition_file();
    void remove_ecu_definition_file();
    void add_new_logger_definition_file();
    void remove_logger_definition_file();
    QString parse_message_to_hex(const QByteArray& received);
    void set_status_bar_label(bool serialConnectionState, bool ecuConnectionState, const QString& romId);
    void custom_menu_requested(QPoint pos);
    void selectable_combobox_item_changed(const QString& item);
    void checkbox_state_changed(int state);
    void close_app();
    // Logger
    // void logger(int log_level, QString message, bool timestamp, bool linefeed);
    // void logger(QString message, bool timestamp, bool linefeed);
    // void sendMsgToLogWindow(QWidget* parent, QString msg);
    void send_message_to_log_window(const QString& msg);
    void network_state_changed(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);

    // logvalues.c
    void change_log_gauge_value(int index);
    void change_log_digital_value(int index);
    void change_log_switch_value(int index);

    void update_vbatt();

  signals:
    void check_serial_port();
    void send_serial_data(QByteArray output);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);
    // void syslog(int logType, bool write_syslog_to_file, QString message, bool timestamp, bool linefeed);
    void syslog(QString message, bool timestamp, bool linefeed);
    void enable_log_write_to_file(bool enable);

  private:
    std::unique_ptr<Ui::MainWindow> ui;
};
