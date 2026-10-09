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

extern void log_error(const QString& message, bool timestamp, bool linefeed);
extern void log_warning(const QString& message, bool timestamp, bool linefeed);
extern void log_info(const QString& message, bool timestamp, bool linefeed);
extern void log_debug(const QString& message, bool timestamp, bool linefeed);

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
    void load_logger_definition();
    void load_logger_selection();
    void save_logger_selection();
    void write_logger_csv_cells(bool header);
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
    std::optional<fastecu::calibration::SessionId> session_of(const QTreeWidgetItem *filesItem) const;
    fastecu::calibration::CalibrationSession *selected_calibration();
    OpenCalibration *open_calibration(fastecu::calibration::SessionId id);
    OpenCalibration *selected_open_calibration();
    void set_category_expanded(QTreeWidgetItem *item, bool expanded);
    QTreeWidgetItem *files_tree_item(fastecu::calibration::SessionId id) const;
    bool add_calibration(fastecu::calibration::SessionId id);

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
    QString selected_serial_port() const
    {
        return serial_ports_.value(serial_port_list_->currentIndex());
    }

    // open_serial_port's bookkeeping once a port opened: forget the ECU when
    // the port changed, and remember the port for the next launch.
    void remember_opened_port(const QString& port, const QString& openedPort);

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
    bool open_calibration_file(QString filename);
    void prompt_for_missing_definition(fastecu::calibration::SessionId id);
    void save_calibration_file();
    void save_calibration_file_as();
    void set_map_selection(fastecu::calibration::SessionId id, int mapIndex, const QString& item);
    void set_map_switch(fastecu::calibration::SessionId id, int mapIndex, int state);
    QStringList parse_stringlist_from_expression_string(QString expression, QString x);
    float calculate_value_from_expression(QStringList expression);

    // log_operations
    void parse_log_value_list(QByteArray received, const QString& protocolArg);
    void log_to_file();

    void setupLoggingEngine();
    void restoreLoggingUiState();

    // logvalues.c
    void change_log_values(int tabIndex, const QString& protocolArg);

    // mainwindow.c
    // Connect signals for any flash class and execute ::run() method
    template <typename FlashClass> FlashClass *connect_signals_and_run_module(FlashClass *object);
    void SetComboBoxItemEnabled(QComboBox *comboBox, int index, bool enabled);
    void set_flash_arrow_state();
    void update_protocol_info(const QString& flashMethod);
    // The session's selected vehicle; always valid once constructed.
    const fastecu::config::VehicleSpec& selected_vehicle() const;
    // Saves the session's settings, logging a failure.
    void save_settings();
    // Emits the LOG_* signal for `level`, with timestamp and linefeed.
    void emit_log_line(fastecu::LogLevel level, const QString& message);
    // Apply a finished dialog's tentative choice: only an accepted one
    // reaches the session. Both then run the matching *_finished slot.
    void apply_vehicle_choice(int result, std::optional<std::size_t> row);
    void apply_protocol_choice(int result, std::optional<std::string> protocolName);
    QStringList create_flash_transports_list();
    QStringList create_log_transports_list();
    // QString check_kernel(QString flash_method);
    QTextEdit *iterateWidgetChild(QObjectList children);
    bool write_syslog(QString msg);

    // menuactions.c
    void connect_menu_actions();
    void apply_standard_shortcuts();
    void show_about_dialog();
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
    // Every entry point that touches the serial facade (disconnect, flash, DTC,
    // BIU, terminal, a port or transport change) first calls
    // connection_coordinator_->cancel(), which can run on_done(false)
    // synchronously. on_done must therefore not start a connection or otherwise
    // touch the facade synchronously; defer any such work to the event loop.
    // See ConnectionCoordinator::begin.
    void connect_to_ecu(std::function<void(bool)> onDone = {});
    void continue_start_logging();

    // What a connection attempt shows and changes in this window. Methods are
    // defined in menu_actions.cpp.
    class ConnectionPresentation final : public fastecu::ui::IConnectionPresentation
    {
      public:
        explicit ConnectionPresentation(MainWindow& window) : window_(window)
        {
        }

        void set_controls_locked(bool locked) override;
        void set_port_selector_enabled(bool enabled) override;
        void identified(const fastecu::ui::IdentifyOutcome& outcome) override;
        void identification_failed(const fastecu::ui::IdentifyOutcome& outcome) override;

      private:
        MainWindow& window_;
    };

    // Declared in this order so the coordinator, which borrows the other two,
    // is destroyed first.
    ConnectionPresentation connection_presentation_{*this};
    std::unique_ptr<fastecu::ui::QtIdentifyLauncher> identify_launcher_;
    std::unique_ptr<fastecu::ui::ConnectionCoordinator> connection_coordinator_;
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

    // mainwindow.c
    void select_protocol();
    void select_protocol_finished(int result);
    void select_vehicle();
    void select_vehicle_finished(int result);
    void log_transport_changed();
    void flash_transport_changed();
    void check_serial_ports();
    void open_serial_port();
    int start_ecu_operations(const QString& cmdType);
    void close_calibration();
    void close_calibration_map(QObject *obj);
    void change_gauge_values();
    void change_digital_values();
    void change_switch_values();
    void update_logboxes(const QString& protocolArg);
    void update_logbox_values(const QString& protocolArg);
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
    std::unique_ptr<Ui::MainWindow> ui_;
};
