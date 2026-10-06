#include "src/ui/desktop/widgets/mainwindow.h"
#include "src/backend/calibration/session/selectable_edit_use_case.h"
#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"
#include "src/ui/desktop/calibration/map_presentation.h"
#include "src/ui/desktop/calibration/map_edit_adapter.h"
#include "src/ui/desktop/calibration/qt_calibration_interaction.h"
#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/ui/desktop/connection/connection_coordinator.h"
#include "src/ui/desktop/widgets/qt_identify_launcher.h"
#include "src/ui/desktop/calibration/rom_info.h"
#include "src/ui/desktop/calibration/session_key.h"
#include "ui_mainwindow.h"
#include <QProgressBar>
#include <QScopeGuard>
#include <QSplashScreen>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/backend/logging/logger_definition_service.h"
#include "src/backend/flash/flash_operation_request.h"
#include "src/platform/desktop/common/flash/flash_workflow.h"
#include "src/platform/desktop/common/serial/serial_idle.h"
#include "src/ui/desktop/config_fields.h"
#include "src/ui/desktop/flash/operation/flash_operation_controller.h"

using fastecu::config::ProtocolSpec;
using fastecu::ui::checksum_field;
using fastecu::ui::kernel_address_field;
using fastecu::ui::protocol_capability;
using fastecu::ui::protocol_field;
using fastecu::ui::qs;

const QColor MainWindow::RED_LIGHT_OFF = QColor(96, 32, 32);
const QColor MainWindow::YELLOW_LIGHT_OFF = QColor(96, 96, 32);
const QColor MainWindow::GREEN_LIGHT_OFF = QColor(32, 96, 32);
const QColor MainWindow::RED_LIGHT_ON = QColor(255, 64, 64);
const QColor MainWindow::YELLOW_LIGHT_ON = QColor(223, 223, 64);
const QColor MainWindow::GREEN_LIGHT_ON = QColor(64, 255, 64);

MainWindow::MainWindow(MainWindowServices services, const QString& peerAddress, QWidget *parent)
    : QMainWindow(parent), services_(services), peerAddress(peerAddress), ui{std::make_unique<Ui::MainWindow>()}
{
    ui->setupUi(this);
    qApp->installEventFilter(this);
    qRegisterMetaType<QVector<int>>("QVector<int>");

    int id = QFontDatabase::addApplicationFont(":/fonts/FastECU_bars.ttf");
    if (id >= 0)
    {
        QString family = QFontDatabase::applicationFontFamilies(id).at(0);
        QFont monospace(family);
    }

    configSession = &services_.config;
    calibrationWorkspace = &services_.calibrations;

    software_name = qs(services_.application.name);
    software_title = qs(services_.application.title);
    software_version = qs(services_.application.version);
    this->setWindowTitle(software_title + " " + software_version);

    log_channel = &services_.log;
    using fastecu::ui::LogChannel;
    QObject::connect(this, &MainWindow::LOG_E, log_channel, &LogChannel::LOG_E);
    QObject::connect(this, &MainWindow::LOG_W, log_channel, &LogChannel::LOG_W);
    QObject::connect(this, &MainWindow::LOG_I, log_channel, &LogChannel::LOG_I);
    QObject::connect(this, &MainWindow::LOG_D, log_channel, &LogChannel::LOG_D);
    QObject::connect(this, &MainWindow::enable_log_write_to_file, log_channel, &LogChannel::enable_log_write_to_file);
    QObject::connect(log_channel, &LogChannel::log_window_message, this, &MainWindow::send_message_to_log_window);

    setupLoggingEngine();

#if defined Q_OS_UNIX
    emit LOG_D("Running on Linux Desktop ", true, false);
    // serialPort = serialPortLinux;
    serial_port_prefix = "/dev/";
#elif defined Q_OS_WIN32
    emit LOG_D("Running on Windows Desktop ", true, false);
    // serialPort = serialPortWindows;
    serial_port_prefix = "";
#endif

#if Q_PROCESSOR_WORDSIZE == 4
    emit LOG_D("32-bit executable", true, true);
#elif Q_PROCESSOR_WORDSIZE == 8
    emit LOG_D("64-bit executable", true, true);
#endif

    QObject::connect(&services_.file_action_events, &QtEventSink::logged, this,
                     [this](int level, const QString& message)
                     { emit_log_line(static_cast<fastecu::LogLevel>(level), message); });
    QObject::connect(&services_.file_action_events, &QtEventSink::noticed, this,
                     [this](QString message) { QMessageBox::warning(this, software_title, message); });

    calibration_interaction_ = std::make_unique<fastecu::ui::QtCalibrationInteraction>(this);
    calibration_operations_ = std::make_unique<fastecu::ui::CalibrationOperationCoordinator>(
        *configSession, services_.rom_save, *calibration_interaction_,
        fastecu::ui::CalibrationPresentationCallbacks{
            .log = [this](fastecu::LogLevel level, std::string_view text)
            { emit_log_line(level, QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()))); },
            .protocol_description_changed =
                [this](std::string_view description)
            {
                status_bar_ecu_label->setText(
                    QString::fromUtf8(description.data(), static_cast<qsizetype>(description.size())) + " ");
            },
        });

    identify_launcher_ = std::make_unique<fastecu::ui::QtIdentifyLauncher>(
        [this] { return std::make_unique<fastecu::diagnostics::SerialDiagnosticLink>(&connection->facade()); },
        [] { return std::make_unique<QtClock>(); },
        [this](fastecu::LogLevel level, const QString& message)
        {
            if (level == fastecu::LogLevel::Warning)
            {
                emit LOG_W(message, true, true);
            }
            else
            {
                emit LOG_D(message, true, true);
            }
        });
    connection_coordinator_ =
        std::make_unique<fastecu::ui::ConnectionCoordinator>(*identify_launcher_, connection_presentation_);

    definitionAuthoringDialog = new fastecu::ui::DefinitionAuthoringDialog(
        services_.definition_catalogs, *configSession, services_.config_repository, this);
    QObject::connect(definitionAuthoringDialog, &fastecu::ui::DefinitionAuthoringDialog::LOG_E, log_channel,
                     &fastecu::ui::LogChannel::LOG_E);
    QObject::connect(definitionAuthoringDialog, &fastecu::ui::DefinitionAuthoringDialog::LOG_W, log_channel,
                     &fastecu::ui::LogChannel::LOG_W);
    QObject::connect(definitionAuthoringDialog, &fastecu::ui::DefinitionAuthoringDialog::LOG_I, log_channel,
                     &fastecu::ui::LogChannel::LOG_I);
    QObject::connect(definitionAuthoringDialog, &fastecu::ui::DefinitionAuthoringDialog::LOG_D, log_channel,
                     &fastecu::ui::LogChannel::LOG_D);

    emit enable_log_write_to_file(true);

    QObject::connect(calibrationTreeWidget, &CalibrationTreeWidget::LOG_E, log_channel,
                     &fastecu::ui::LogChannel::LOG_E);
    QObject::connect(calibrationTreeWidget, &CalibrationTreeWidget::LOG_W, log_channel,
                     &fastecu::ui::LogChannel::LOG_W);
    QObject::connect(calibrationTreeWidget, &CalibrationTreeWidget::LOG_I, log_channel,
                     &fastecu::ui::LogChannel::LOG_I);
    QObject::connect(calibrationTreeWidget, &CalibrationTreeWidget::LOG_D, log_channel,
                     &fastecu::ui::LogChannel::LOG_D);

    // Before this window was built, DesktopComposition initialized the session
    // and the startup vehicle gate made sure a vehicle is selected.
    emit LOG_D("Vehicle ID: " + qs(configSession->settings().selected_vehicle_id) + "/" +
                   QString::number(configSession->vehicles().size()),
               true, true);

    emit LOG_D(qs(selected_vehicle().make), true, true);
    emit LOG_D(protocol_field(selected_vehicle(), &ProtocolSpec::mcu), true, true);
    emit LOG_D(checksum_field(selected_vehicle()), true, true);
    emit LOG_D(qs(selected_vehicle().model), true, true);
    emit LOG_D(qs(selected_vehicle().version), true, true);
    emit LOG_D(qs(selected_vehicle().protocol->name), true, true);
    emit LOG_D(protocol_field(selected_vehicle(), &ProtocolSpec::description), true, true);
    emit LOG_D(qs(configSession->settings().selected_flash_transport), true, true);
    emit LOG_D(qs(configSession->settings().selected_log_transport), true, true);
    emit LOG_D(qs(configSession->settings().selected_log_protocol), true, true);
    emit LOG_D("ECU protocols set", true, true);

    QRect qrect = MainWindow::geometry();

    if (const fastecu::config::AppConfig& window_settings = configSession->settings();
        window_settings.window_width != "maximized" && window_settings.window_height != "maximized")
    {
        this->setGeometry(qrect.x(), qrect.y(), qs(window_settings.window_width).toInt(),
                          qs(window_settings.window_height).toInt());
    }
    else
    {
        this->setWindowState(Qt::WindowMaximized);
    }

    if (configSession->settings().romraider_definition_files.empty() &&
        configSession->settings().ecuflash_definition_files_directory.empty())
    {
        QMessageBox::warning(this, tr("Ecu definition file"),
                             "No definition file(s), use 'Settings' in 'Edit' menu to choose file(s)");
    }

    // Scan errors are nonfatal and already reported by the session.
    std::ignore = services_.definition_catalogs.refresh_index(fastecu::definition::DefinitionFormat::EcuFlash);

    // Scan errors are nonfatal and already reported by the session.
    std::ignore = services_.definition_catalogs.refresh_index(fastecu::definition::DefinitionFormat::RomRaider);

    if (const QString kernel_dir = qs(configSession->effective_paths().kernel_files_directory);
        QDir(kernel_dir).exists())
    {
        QDir dir(kernel_dir);
        QStringList nameFilter("*.bin");
        QStringList txtFilesAndDirectories = dir.entryList(nameFilter);
        // emit LOG_D(txtFilesAndDirectories;
    }

    apply_standard_shortcuts();
    connect_menu_actions();

    connect(ui->switchBoxWidget, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(change_switch_values()));
    connect(ui->logBoxWidget, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(change_digital_values()));
    connect(ui->mdiArea, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(change_gauge_values()));
    // connect(ui->action_Preferences, SIGNAL(triggered()), this, SLOT(openPreferences()));
    connect(ui->calibrationFilesTreeWidget, SIGNAL(itemClicked(QTreeWidgetItem *, int)), this,
            SLOT(calibration_files_treewidget_item_selected(QTreeWidgetItem *)));
    connect(ui->calibrationDataTreeWidget, SIGNAL(itemClicked(QTreeWidgetItem *, int)), this,
            SLOT(calibration_data_treewidget_item_selected(QTreeWidgetItem *)));
    connect(ui->calibrationDataTreeWidget, SIGNAL(itemExpanded(QTreeWidgetItem *)), this,
            SLOT(calibration_data_treewidget_item_expanded(QTreeWidgetItem *)));
    connect(ui->calibrationDataTreeWidget, SIGNAL(itemCollapsed(QTreeWidgetItem *)), this,
            SLOT(calibration_data_treewidget_item_collapsed(QTreeWidgetItem *)));
    connect(calibrationTreeWidget, SIGNAL(closeRom()), this, SLOT(close_calibration()));

    status_bar_connection_label->setMargin(5);
    // status_bar_connection_label->setStyleSheet("QLabel { background-color : red; color : white; }");
    set_status_bar_label(false, false, "");

    status_bar_ecu_label->setText("");
    status_bar_ecu_label->setMargin(5);
    status_bar_ecu_label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    // status_bar_ecu_label->setStyleSheet("QLabel { background-color : red; color : white; }");

    QWidget *status_bar_spacer = new QWidget();
    status_bar_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    statusBar()->addWidget(status_bar_connection_label);
    statusBar()->addWidget(status_bar_spacer);
    statusBar()->addPermanentWidget(status_bar_ecu_label);
    statusBar()->setSizeGripEnabled(true);

    ui->calibrationFilesTreeWidget->setHeaderLabel("Calibration Files");
    ui->calibrationDataTreeWidget->setHeaderLabel("Calibration Data");
    ui->calibrationDataTreeWidget->resizeColumnToContents(0);
    ui->calibrationDataTreeWidget->resizeColumnToContents(1);
    ui->calibrationFilesTreeWidget->setMinimumHeight(125);
    ui->calibrationFilesTreeWidget->minimumHeight();
    ui->calibrationFilesTreeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->calibrationFilesTreeWidget, SIGNAL(customContextMenuRequested(QPoint)),
            SLOT(custom_menu_requested(QPoint)));

    // ui->splitter->setStretchFactor(2, 1);
    ui->splitter->setSizes(QList<int>({125, INT_MAX}));

    // Splash screen
    netSplash = new QSplashScreen();
    QVBoxLayout *netSplashLayout = new QVBoxLayout(netSplash);
    netSplashLayout->setAlignment(Qt::AlignCenter);
    QLabel *netSplashLabel = new QLabel(QString("Waiting for peer " + peerAddress + "..."), netSplash);
    netSplashLabel->setAlignment(Qt::AlignCenter);
    QProgressBar *netSplashProgressBar = new QProgressBar(netSplash);
    netSplashProgressBar->setAlignment(Qt::AlignCenter);
    netSplashProgressBar->setMinimum(0);
    // Number of network connection stages
    netSplashProgressBar->setMaximum(2);
    netSplashProgressBar->setValue(0);
    QPushButton *btnCloseApp = new QPushButton("Close app", netSplash);
    netSplashLayout->addWidget(netSplashLabel);
    netSplashLayout->addWidget(netSplashProgressBar);
    netSplashLayout->addWidget(btnCloseApp);
    // splash->setLayout(layout);
    netSplash->resize(350, 50);
    // Show it in remote mode only
    // Prepare for remote mode
    if (!peerAddress.isEmpty())
    {
        netSplash->show();
        // Move splashscreen to the center of the screen
        QScreen *screen_local = QGuiApplication::primaryScreen();
        QRect screenGeometry_local = screen_local->geometry();
        netSplash->move(screenGeometry_local.center() - netSplash->rect().center());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

        QString wt = this->windowTitle();
        if (peerAddress.length() > 0)
        {
            wt += " - Remote Connection to " + peerAddress;
        }
        this->setWindowTitle(wt);
    }
    // Add option to close app while waiting for network connection
    QObject::connect(btnCloseApp, &QPushButton::released, this,
                     [&]()
                     {
                         netSplashLabel->setText("Closing app, please wait...");
                         exit(1);
                     });

    // Init may take a long time due to network
    // Do it in a non-blocking way
    // This timer will timeout as fast as possible
    // processing events
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [&]() { QApplication::processEvents(); });
    timer->start();

    connection = &services_.connection;
    remote_peer = &services_.remote;
    if (!peerAddress.isEmpty())
    {
        netSplashProgressBar->setValue(0);
        netSplashProgressBar->setFormat("Connecting to J2534 and serial devices...");
        connection->wait_for_source();
        netSplashProgressBar->setValue(1);
        netSplashProgressBar->setFormat("Connecting to utility functions...");
        remote_peer->wait_for_source();
        netSplashProgressBar->setValue(2);
    }
    external_logger("Connection successfull.");

    timer->stop();
    netSplash->close();
    timer->deleteLater();
    connect(connection, &fastecu::desktop::connection::AdapterConnection::stateChanged, this,
            &MainWindow::network_state_changed, Qt::DirectConnection);
    connect(remote_peer, &fastecu::ui::RemotePeer::stateChanged, this, &MainWindow::network_state_changed,
            Qt::DirectConnection);

    // Set timer to read vbatt value
    vbatt_timer = new QTimer(this);
    vbatt_timer->setInterval(vbatt_timer_timeout);
    connect(vbatt_timer, SIGNAL(timeout()), this, SLOT(update_vbatt()));

    toolbar_item_size.setWidth(qs(configSession->settings().toolbar_iconsize).toInt());
    toolbar_item_size.setHeight(qs(configSession->settings().toolbar_iconsize).toInt());
    ui->toolBar->setIconSize(toolbar_item_size);

    QWidget *spacer = new QWidget();
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->toolBar->addWidget(spacer);

    ui->toolBar->addSeparator();

    QPushButton *select_protocol_button = new QPushButton();
    select_protocol_button->setText("Select protocol");
    // car_make_button->setMargin(10);
    select_protocol_button->setFixedHeight(toolbar_item_size.height());
    ui->toolBar->addWidget(select_protocol_button);
    connect(select_protocol_button, SIGNAL(clicked(bool)), this, SLOT(select_protocol()));

    QPushButton *select_vehicle_button = new QPushButton();
    select_vehicle_button->setText("Select vehicle");
    // car_make_button->setMargin(10);
    select_vehicle_button->setFixedHeight(toolbar_item_size.height());
    ui->toolBar->addWidget(select_vehicle_button);
    connect(select_vehicle_button, SIGNAL(clicked(bool)), this, SLOT(select_vehicle()));

    flash_transport_list = new QComboBox();
    flash_transport_list->setFixedHeight(toolbar_item_size.height());
    flash_transport_list->setFixedWidth(90);
    flash_transport_list->setObjectName("flash_transport_list");
    ui->toolBar->addWidget(flash_transport_list);

    ui->toolBar->addSeparator();

    QLabel *log_transport = new QLabel("Log:");
    log_transport->setMargin(10);
    ui->toolBar->addWidget(log_transport);

    log_transport_list = new QComboBox();
    log_transport_list->setFixedHeight(toolbar_item_size.height());
    log_transport_list->setFixedWidth(90);
    log_transport_list->setObjectName("log_transport_list");
    ui->toolBar->addWidget(log_transport_list);

    flash_transports = create_flash_transports_list();
    log_transports = create_log_transports_list();
    connect(flash_transport_list, SIGNAL(currentIndexChanged(int)), this, SLOT(flash_transport_changed()));
    connect(log_transport_list, SIGNAL(currentIndexChanged(int)), this, SLOT(log_transport_changed()));

    // ui->toolBar->addSeparator();

    QLabel *log_select = new QLabel();
    log_select->setMargin(5);
    ui->toolBar->addWidget(log_select);

    ecu_radio_button = new QRadioButton("ECU");
    ecu_radio_button->setChecked(true);
    ui->toolBar->addWidget(ecu_radio_button);
    tcu_radio_button = new QRadioButton("TCU");
    ui->toolBar->addWidget(tcu_radio_button);

    ui->toolBar->addSeparator();

    QLabel *serial_port_select = new QLabel("Port:");
    serial_port_select->setMargin(10);
    ui->toolBar->addWidget(serial_port_select);

    serial_port_list = new QComboBox();
    serial_port_list->setFixedHeight(toolbar_item_size.height());
    serial_port_list->setFixedWidth(180);
    serial_port_list->setObjectName("serial_port_list");
    serial_ports = connection->available_ports();
    for (int i = 0; i < serial_ports.length(); i++)
    {
        serial_port_list->addItem(serial_ports.at(i));
        if (qs(configSession->settings().serial_port) == serial_ports.at(i).split(" - ").at(0))
        {
            serial_port_list->setCurrentIndex(i);
        }
    }
    ui->toolBar->addWidget(serial_port_list);

    refresh_serial_port_list = new QPushButton();
    refresh_serial_port_list->setIcon(QIcon(":/icons/view-refresh.png"));
    refresh_serial_port_list->setFixedHeight(toolbar_item_size.height());
    refresh_serial_port_list->setFixedWidth(toolbar_item_size.height());
    // refresh_serial_port_list->setIconSize(toolbar_item_size);
    connect(refresh_serial_port_list, SIGNAL(clicked(bool)), this, SLOT(check_serial_ports()));
    ui->toolBar->addWidget(refresh_serial_port_list);

    loggerModel = &services_.logger_model;
    load_logger_definition();
    loggerValues.initialize(*loggerModel);
    logBoxes = new LogBox();

    if (loggerModel != nullptr)
    {
        update_logboxes(qs(configSession->settings().selected_log_protocol));
    }

    serial_port = serial_port_prefix + qs(configSession->settings().serial_port);
    serial_port_baudrate = default_serial_port_baudrate;
    connection->set_initial_port(serial_port, serial_port_baudrate);
    /*
        serial_poll_timer = new QTimer(this);
        serial_poll_timer->setInterval(serial_poll_timer_timeout);
        connect(serial_poll_timer, SIGNAL(timeout()), this, SLOT(open_serial_port()));
        serial_poll_timer->start();

        ssm_init_poll_timer = new QTimer(this);
        ssm_init_poll_timer->setInterval(ssm_init_poll_timer_timeout);
        connect(ssm_init_poll_timer, SIGNAL(timeout()), this, SLOT(ecu_init()));
        ssm_init_poll_timer->start();
    */
    log_file_timer = std::make_unique<QElapsedTimer>();

    if (!calibrations_.empty())
    {
        const QModelIndex index = ui->calibrationFilesTreeWidget->selectionModel()->currentIndex();
        emit ui->calibrationFilesTreeWidget->clicked(index);
    }

    emit log_transport_list->currentIndexChanged(log_transport_list->currentIndex());

    status_bar_ecu_label->setText(protocol_field(selected_vehicle(), &ProtocolSpec::description) + " ");

    set_flash_arrow_state();

    netSplash->deleteLater();
    emit LOG_I("FastECU initialized", true, true);
}

void MainWindow::emit_log_line(fastecu::LogLevel level, const QString& message)
{
    switch (level)
    {
    case fastecu::LogLevel::Error:
        emit LOG_E(message, true, true);
        break;
    case fastecu::LogLevel::Warning:
        emit LOG_W(message, true, true);
        break;
    case fastecu::LogLevel::Info:
        emit LOG_I(message, true, true);
        break;
    case fastecu::LogLevel::Debug:
        emit LOG_D(message, true, true);
        break;
    }
}

MainWindow::~MainWindow()
{
    connection_coordinator_->shutdown();
    if (logging_state)
    {
        loggingEngine->stop();
    }
}

void MainWindow::network_state_changed(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState)
{
    if (state == QRemoteObjectReplica::Valid)
    {
        emit LOG_D("Network connection established", true, true);
    }
    else if (oldState == QRemoteObjectReplica::Valid)
    {
        if (!restartQuestionActive.tryLock())
        {
            return;
        }
        emit LOG_D("Network connection lost, reconnecting...", true, true);
        QMessageBox msgBox;
        msgBox.setText("Network connection lost.");
        msgBox.setInformativeText("Do you want to restart application and try to connect again?");
        QPushButton *restartButton = msgBox.addButton(tr("Restart"), QMessageBox::YesRole);
        QPushButton *quitButton = msgBox.addButton(tr("Quit"), QMessageBox::NoRole);
        msgBox.addButton(tr("Continue work"), QMessageBox::NoRole);
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setDetailedText("Press Restart to restart application. All your unsaved progress will be lost.\n\n"
                               "Press Quit to quit application. All your unsaved progress will be lost.\n\n"
                               "Press Continue work to return to application and save your progress. "
                               "In this case all ECU operations will be unavailable until you restart application.");

        msgBox.exec();

        if (msgBox.clickedButton() == restartButton)
        {
            qApp->exit(kRestartCode);
        }
        else if (msgBox.clickedButton() == quitButton)
        {
            qApp->exit(1);
        }

        restartQuestionActive.unlock();
    }
}

void MainWindow::SetComboBoxItemEnabled(QComboBox *comboBox, int index, bool enabled)
{
    auto *model = qobject_cast<QStandardItemModel *>(comboBox->model());
    assert(model);
    if (!model)
    {
        return;
    }

    auto *item = model->item(index);
    assert(item);
    if (!item)
    {
        return;
    }
    item->setEnabled(enabled);
}

QStringList MainWindow::create_flash_transports_list()
{
    QStringList flash_protocols;

    flash_protocols.append(protocol_field(selected_vehicle(), &ProtocolSpec::flash_transport).split(","));

    flash_transport_list->clear();
    for (int i = 0; i < flash_protocols.length(); i++)
    {
        flash_transport_list->addItem(flash_protocols.at(i));
        if (qs(configSession->settings().selected_flash_transport) == flash_protocols.at(i))
        {
            flash_transport_list->setCurrentIndex(i);
        }
    }
    return flash_protocols;
}

QStringList MainWindow::create_log_transports_list()
{
    QStringList log_transports_local;

    log_transports_local.append(protocol_field(selected_vehicle(), &ProtocolSpec::log_transport).split(","));

    log_transport_list->clear();
    for (int i = 0; i < log_transports_local.length(); i++)
    {
        log_transport_list->addItem(log_transports_local.at(i));
        if (qs(configSession->settings().selected_flash_transport) == log_transports_local.at(i))
        {
            log_transport_list->setCurrentIndex(i);
            protocol = "SSM";
        }
    }

    // if (car_model_list->currentText() == "Subaru")
    // protocol = "SSM";

    return log_transports_local;
}

const fastecu::config::VehicleSpec& MainWindow::selected_vehicle() const
{
    // The startup vehicle gate selects a vehicle before MainWindow is built,
    // and a selection only ever changes to another valid row.
    return *configSession->selected_vehicle();
}

void MainWindow::save_settings()
{
    if (const fastecu::Status saved = configSession->save(); !saved.has_value())
    {
        if (last_settings_save_error != saved.error())
        {
            last_settings_save_error = saved.error();
            emit LOG_E(qs(saved.error().detail), true, true);
        }
    }
    else
    {
        last_settings_save_error.reset();
    }
}

void MainWindow::apply_vehicle_choice(int result, std::optional<std::size_t> row)
{
    if (result == QDialog::Accepted && row.has_value())
    {
        if (const fastecu::Status selected = configSession->select_row(*row); !selected.has_value())
        {
            emit LOG_E(qs(selected.error().detail), true, true);
        }
    }
    select_vehicle_finished(result);
}

void MainWindow::apply_protocol_choice(int result, std::optional<std::string> protocol_name)
{
    if (result == QDialog::Accepted && protocol_name.has_value())
    {
        configSession->select_by_protocol_name(*protocol_name);
    }
    select_protocol_finished(result);
}

void MainWindow::select_protocol()
{
    ProtocolSelect protocolSelect(*configSession);
    const int result = protocolSelect.exec();
    apply_protocol_choice(result, protocolSelect.chosen_protocol_name());
    emit LOG_D("Selected vehicle: " + qs(configSession->settings().selected_vehicle_id), true, true);
}

void MainWindow::select_protocol_finished(int result)
{
    if (result == QDialog::Accepted)
    {
        create_flash_transports_list();
        create_log_transports_list();
        save_settings();

        set_flash_arrow_state();
    }
    else
    {
        // emit LOG_D("Dialog is rejected";
    }

    status_bar_ecu_label->setText(protocol_field(selected_vehicle(), &ProtocolSpec::description) + " ");
}

void MainWindow::select_vehicle()
{
    VehicleSelect vehicleSelect(*configSession);
    const int result = vehicleSelect.exec();
    apply_vehicle_choice(result, vehicleSelect.chosen_row());
    emit LOG_D("Selected vehicle: " + qs(configSession->settings().selected_vehicle_id), true, true);
}

void MainWindow::select_vehicle_finished(int result)
{
    if (result == QDialog::Accepted)
    {
        create_flash_transports_list();
        create_log_transports_list();
        save_settings();

        set_flash_arrow_state();
    }
    else
    {
        // emit LOG_D("Dialog is rejected";
    }

    status_bar_ecu_label->setText(protocol_field(selected_vehicle(), &ProtocolSpec::description) + " ");
}

MainWindow::OpenCalibration *MainWindow::open_calibration(fastecu::calibration::SessionId id)
{
    const auto found = std::ranges::find_if(calibrations_, [id](const OpenCalibration& open) { return open.id == id; });
    return found == calibrations_.end() ? nullptr : &*found;
}

MainWindow::OpenCalibration *MainWindow::selected_open_calibration()
{
    const QList<QTreeWidgetItem *> selected = ui->calibrationFilesTreeWidget->selectedItems();
    const auto id = selected.isEmpty() ? std::nullopt : session_of(selected.at(0));
    return id.has_value() ? open_calibration(*id) : nullptr;
}

fastecu::calibration::CalibrationSession *MainWindow::calibration(fastecu::calibration::SessionId id)
{
    OpenCalibration *open = open_calibration(id);
    return open != nullptr ? calibrationWorkspace->find(open->id) : nullptr;
}

std::optional<fastecu::calibration::SessionId> MainWindow::session_of(const QTreeWidgetItem *files_item) const
{
    return files_item == nullptr ? std::nullopt : fastecu::ui::parse_session_key(files_item->text(2));
}

fastecu::calibration::CalibrationSession *MainWindow::selected_calibration()
{
    OpenCalibration *open = selected_open_calibration();
    return open != nullptr ? calibrationWorkspace->find(open->id) : nullptr;
}

QTreeWidgetItem *MainWindow::files_tree_item(fastecu::calibration::SessionId id) const
{
    for (int i = 0; i < ui->calibrationFilesTreeWidget->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem *item = ui->calibrationFilesTreeWidget->topLevelItem(i);
        if (session_of(item) == id)
        {
            return item;
        }
    }
    return nullptr;
}

// Presents the newly opened session with the protocol refresh, missing-
// definition prompt, and both trees in the established order.
bool MainWindow::add_calibration(fastecu::calibration::SessionId id)
{
    const fastecu::calibration::CalibrationSession *session = calibrationWorkspace->find(id);
    if (session == nullptr)
    {
        return false;
    }
    calibrations_.push_back(OpenCalibration{.id = id});

    update_protocol_info(
        fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(*session), fastecu::ui::RomInfoRow::FlashMethod));
    if (session->definition() == nullptr)
    {
        prompt_for_missing_definition(id);
    }
    const OpenCalibration *open = open_calibration(id);
    calibrationTreeWidget->buildCalibrationFilesTree(id, ui->calibrationFilesTreeWidget, *session);
    calibrationTreeWidget->buildCalibrationDataTree(ui->calibrationDataTreeWidget, *session, open->view);
    return true;
}

void MainWindow::update_protocol_info(const QString& flash_method)
{
    // Mirrored in CalibrationOperationCoordinator::refresh_write_metadata; keep the two in sync.
    emit LOG_D("Update protocol info by selected ROM with FlashMethod: " + flash_method, true, true);
    // The last matching row wins, as the legacy scan did; no match changes
    // nothing.
    if (const bool info_updated = configSession->select_by_protocol_name(flash_method.toStdString()); info_updated)
    {
        emit LOG_D("Protocol info for selected ROM updated", true, true);
    }
    else
    {
        emit LOG_D("Could not find protocol for selected ROM!", true, true);
    }
    status_bar_ecu_label->setText(protocol_field(selected_vehicle(), &ProtocolSpec::description) + " ");
}

void MainWindow::set_flash_arrow_state()
{
    ui->actionReadRomFromEcu->setEnabled(protocol_capability(selected_vehicle(), &ProtocolSpec::read));
    ui->actionTestWriteRomToEcu->setEnabled(protocol_capability(selected_vehicle(), &ProtocolSpec::test_write));
    ui->actionWriteRomToEcu->setEnabled(protocol_capability(selected_vehicle(), &ProtocolSpec::write));
}

void MainWindow::log_transport_changed()
{
    connection_coordinator_->cancel();
    // emit LOG_D("Change log transport";
    QComboBox *log_transport_list = ui->toolBar->findChild<QComboBox *>("log_transport_list");

    connection->apply_log_transport(
        fastecu::desktop::connection::log_transport_from_text(log_transport_list->currentText()),
        configSession->settings().selected_log_protocol == "SSM");

    protocol = qs(configSession->settings().selected_log_protocol);
    configSession->settings().selected_log_transport = log_transport_list->currentText().toStdString();
    save_settings();

    ecuid.clear();
    ecu_init_complete = false;
    // ssm_init_poll_timer->start();
}

void MainWindow::flash_transport_changed()
{
    // emit LOG_D("Change flash transport";
    QComboBox *flash_transport_list = ui->toolBar->findChild<QComboBox *>("flash_transport_list");

    configSession->settings().selected_flash_transport = flash_transport_list->currentText().toStdString();
    save_settings();
}

void MainWindow::check_serial_ports()
{
    connection_coordinator_->cancel();
    QComboBox *serial_port_list = ui->toolBar->findChild<QComboBox *>("serial_port_list");
    QString prev_serial_port = serial_port_list->currentText();
    int index = 0;

    // serial_poll_timer->stop();
    // ssm_init_poll_timer->stop();

    connection->clear_link_flags();
    ecuid.clear();
    ecu_init_complete = false;
    emit log_transport_list->currentIndexChanged(log_transport_list->currentIndex());

    // QStringList j2534_list = serial->getAvailableJ2534Libs();
    // emit LOG_D("J2534 Vehicle PassThru Interfaces:" << j2534_list;

    serial_ports = connection->available_ports();
    serial_port_list->clear();

    for (int i = 0; i < serial_ports.length(); i++)
    {
        serial_port_list->addItem(serial_ports.at(i));
        if (prev_serial_port == serial_ports.at(i))
        {
            serial_port_list->setCurrentIndex(index);
        }
        index++;
    }

    // emit LOG_D("Start serial and ssm poll timers";
    // serial_poll_timer->start();
    // ssm_init_poll_timer->start();
}

void MainWindow::open_serial_port()
{
    connection_coordinator_->cancel();
    const QString port = selected_serial_port();
    if (port.isEmpty())
    {
        return;
    }
    connection->select_port(port);
    QString opened_serial_port = connection->open();
    if (opened_serial_port != "")
    {
        remember_opened_port(port, opened_serial_port);
        if (ecuid == "")
        {
            set_status_bar_label(true, false, "");
        }
        else
        {
            set_status_bar_label(true, true, ecuid);
        }
    }
    else
    {
        set_status_bar_label(false, false, "");
        ecu_init_complete = false;
    }
}

void MainWindow::remember_opened_port(const QString& port, const QString& opened_port)
{
    if (opened_port != previous_serial_port)
    {
        ecuid.clear();
        ecu_init_complete = false;
    }
    previous_serial_port = opened_port;
    configSession->settings().serial_port = port.toStdString();
    save_settings();
}

int MainWindow::start_ecu_operations(const QString& cmd_type)
{
    connection_coordinator_->cancel();
    set_realtime_state(false);
    toggle_realtime();

    std::optional<fastecu::ui::PreparedWrite> prepared_write;
    QString read_kernel_path;
    QString read_kernel_address;

    QComboBox *serial_port_list = ui->toolBar->findChild<QComboBox *>("serial_port_list");
    if (serial_port_list->currentText() == "")
    {
        emit LOG_D("No serial port selected!", true, true);
        QMessageBox::warning(this, tr("Serial port"), "No serial port selected!");
        return 0;
    }

    connection->select_port(selected_serial_port());

    // A local copy: the provisioned kernel directory is never rewritten.
    QString kernel_dir = qs(configSession->effective_paths().kernel_files_directory);
    if (!kernel_dir.endsWith('/'))
    {
        kernel_dir.append("/");
    }

    // Every path from here on -- including the write-preflight early returns
    // and the Denso TCU service-action return -- restores the serial facade
    // and stops battery polling on exit.
    const auto cleanup = qScopeGuard(
        [this]
        {
            vbatt_timer->stop();
            fastecu::desktop::serial::reset_serial_to_idle(connection->facade());
            ecuid.clear();
            ecu_init_complete = false;
            emit log_transport_list->currentIndexChanged(log_transport_list->currentIndex());
            connection->set_port_speed(4800);
        });

    if (selected_vehicle().make == "Subaru" || selected_vehicle().make == "Mitsubishi")
    {
        fastecu::desktop::serial::reset_serial_to_idle(connection->facade());
        ecuid.clear();
        ecu_init_complete = false;

        update_vbatt();
        vbatt_timer->start();

        if (!kernel_dir.endsWith('/'))
        {
            kernel_dir.append("/");
        }

        if (cmd_type == "test_write" || cmd_type == "write")
        {
            prepared_write = calibration_operations_->prepare_write(selected_calibration(), kernel_dir.toStdString());
            if (!prepared_write.has_value())
            {
                return 0;
            }
        }
        else
        {
            // Nothing is allocated before the read: the image is adopted into
            // a session only after it succeeded.
            update_protocol_info(qs(selected_vehicle().protocol->name));
            read_kernel_path = QString::fromStdString(
                fastecu::flash::kernel_path(kernel_dir.toStdString(), selected_vehicle().protocol->kernel));
            read_kernel_address = kernel_address_field(selected_vehicle());
        }

        emit LOG_D("Protocol to use: " + qs(selected_vehicle().protocol->name), true, true);

        const fastecu::flash::FlashOperation operation =
            fastecu::flash::flash_operation_from_command(cmd_type.toStdString());

        fastecu::flash::FlashOperationController controller{connection->facade(), this};
        // Relay through MainWindow's own LOG_* signals, like every UI logger;
        // see LogChannel for why lines go through a long-lived sender.
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::LOG_E, this, &MainWindow::LOG_E);
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::LOG_W, this, &MainWindow::LOG_W);
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::LOG_I, this, &MainWindow::LOG_I);
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::LOG_D, this, &MainWindow::LOG_D);
        QObject::connect(&controller, qOverload<QString>(&fastecu::flash::FlashOperationController::external_logger),
                         this, &MainWindow::external_logger);
        QObject::connect(&controller, qOverload<int>(&fastecu::flash::FlashOperationController::external_logger), this,
                         &MainWindow::external_logger_set_progressbar_value);

        const fastecu::flash::FlashOperationOutcome outcome = controller.run({
            .operation = operation,
            .protocol = prepared_write.has_value() ? prepared_write->protocol : *selected_vehicle().protocol,
            .kernel_path = prepared_write.has_value() ? prepared_write->kernel_path : read_kernel_path.toStdString(),
            .image = fastecu::flash::portableImageForOperation(
                operation, prepared_write.has_value() ? bytes::ByteView{prepared_write->image} : bytes::ByteView{}),
            .paths = configSession->effective_paths(),
            .display_filename = prepared_write.has_value() ? prepared_write->display_filename : std::string{},
        });

        if (outcome.status == fastecu::flash::FlashOperationStatus::ServiceActionHandled)
        {
            // The old goto skipped the post-operation block entirely.
        }
        else if (cmd_type == "read")
        {
            if (outcome.status == fastecu::flash::FlashOperationStatus::Completed && outcome.read_bytes &&
                !outcome.read_bytes->empty())
            {
                const QString dateTimeString = QDateTime::currentDateTime().toString("yyyy-MM-dd_hh'h'mm'm'ss's'");
                const std::string rom_id = outcome.rom_id.value_or(std::string{});
                const auto opened = calibrationWorkspace->adopt_read_image(fastecu::calibration::ReadImage{
                    .rom = *outcome.read_bytes,
                    .filename = fastecu::flash::read_image_filename(rom_id, dateTimeString.toStdString()),
                    .rom_id = rom_id,
                    .protocol_name = std::string(selected_vehicle().protocol->name),
                    .kernel_path = read_kernel_path.toStdString(),
                    .kernel_start_address = read_kernel_address.toStdString(),
                });
                if (opened.has_value() && add_calibration(opened->id))
                {
                    save_calibration_file_as();
                }
            }
        }
    }
    return 0;
}

void MainWindow::custom_menu_requested(QPoint pos)
{
    if (calibrations_.empty())
    {
        emit LOG_D("No calibration to select!", true, true);
        return;
    }
    QModelIndex index = ui->calibrationFilesTreeWidget->indexAt(pos);
    emit ui->calibrationFilesTreeWidget->clicked(index);

    QMenu *menu = new QMenu(this);
    // menu->addAction("Sync with ECU", this, SLOT(syncCalWithEcu()));
    menu->addAction("Close selected ROM", this, SLOT(close_calibration()));
    menu->popup(ui->calibrationFilesTreeWidget->viewport()->mapToGlobal(pos));
}

bool MainWindow::open_calibration_file(QString filename)
{
    if (filename.isEmpty())
    {
        QFileDialog openDialog;
        openDialog.setDefaultSuffix("bin");
        filename = QFileDialog::getOpenFileName(this, tr("Open ROM file"),
                                                qs(configSession->effective_paths().calibration_files_directory),
                                                tr("Calibration file (*.bin *.hex)"));
        if (filename.isEmpty())
        {
            QMessageBox::information(this, tr("Calibration file"), "No file selected");
            return 1; // matches open_subaru_rom_file's old "no file selected" return of nullptr -> caller's failure
                      // path
        }
    }

    const auto opened = calibrationWorkspace->open_file(filename.toStdString());
    if (!opened.has_value())
    {
        return 1;
    }
    return add_calibration(opened->id) ? 0 : 1;
}

void MainWindow::prompt_for_missing_definition(fastecu::calibration::SessionId id)
{
    QDialog *definitionDialog = new QDialog(this);
    QVBoxLayout *vBoxLayout = new QVBoxLayout(definitionDialog);
    QLabel *label = new QLabel("Unable to find definition for selected ROM file!\n\nSelect option:");
    QRadioButton *createNewRadioButton = new QRadioButton("Create new definition file template");
    QRadioButton *useExistingRadioButton = new QRadioButton("Use existing definition file as base");
    QRadioButton *continueWithoutRadioButton = new QRadioButton("Continue without definition file");

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, definitionDialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, definitionDialog, &QDialog::reject);

    vBoxLayout->addWidget(label);
    vBoxLayout->addWidget(createNewRadioButton);
    vBoxLayout->addWidget(useExistingRadioButton);
    vBoxLayout->addWidget(continueWithoutRadioButton);
    vBoxLayout->addWidget(buttonBox);
    continueWithoutRadioButton->setChecked(true);

    int result = definitionDialog->exec();
    if (result == QDialog::Accepted)
    {
        if (createNewRadioButton->isChecked())
        {
            emit LOG_D(createNewRadioButton->text(), true, true);
            definitionAuthoringDialog->create_new_definition();
        }
        else if (useExistingRadioButton->isChecked())
        {
            emit LOG_D(useExistingRadioButton->text(), true, true);
            definitionAuthoringDialog->use_existing_definition();
        }
    }
    // The "continue without definition" placeholders belong to this branch
    // only -- a user who picked create-new or use-existing must not get them
    // stamped over the definition they just provided.
    if (continueWithoutRadioButton->isChecked() || result == QDialog::Rejected)
    {
        emit LOG_D(continueWithoutRadioButton->text(), true, true);
        if (OpenCalibration *open = open_calibration(id); open != nullptr)
        {
            open->view.missing_definition_make = qs(selected_vehicle().make);
        }
    }
}

void MainWindow::save_calibration_file()
{
    // The coordinator reports every outcome; saving in place changes no label.
    calibration_operations_->save(selected_calibration(), fastecu::ui::SaveMode::Save);
}

void MainWindow::save_calibration_file_as()
{
    // Resolved once: the selection can change while the picker is open, and
    // the label belongs to the session that was saved.
    fastecu::calibration::CalibrationSession *session = selected_calibration();
    if (calibration_operations_->save(session, fastecu::ui::SaveMode::SaveAs) != fastecu::ui::SaveOutcome::Saved)
    {
        return;
    }
    if (QTreeWidgetItem *item = files_tree_item(session->id()); item != nullptr)
    {
        item->setText(0, qs(session->source().display_name));
    }
}

void MainWindow::selectable_combobox_item_changed(const QString& item)
{
    const auto id = fastecu::ui::parse_map_window_id(ui->mdiArea->activeSubWindow());
    if (id.has_value())
    {
        set_map_selection(id->session, id->map_number, item);
    }
}

void MainWindow::set_map_selection(fastecu::calibration::SessionId id, int map_index, const QString& item)
{
    if (map_index < 0)
    {
        return;
    }
    const auto outcome = fastecu::calibration::apply_selectable_edit(
        *calibrationWorkspace,
        {.session = id, .map_index = static_cast<std::size_t>(map_index), .selection = item.toStdString()});
    if (!outcome.has_value())
    {
        QMessageBox::warning(this, tr("Set value"), qs(outcome.error().detail));
        return;
    }
    // A name no selection carries still refreshes, so the combo snaps back to the ROM's bytes.
    if (const auto *skipped = std::get_if<fastecu::calibration::SelectableEditNotApplicable>(&*outcome);
        skipped != nullptr && skipped->reason != fastecu::calibration::SelectableNotApplicableReason::UnknownSelection)
    {
        return;
    }
    for (auto *window : ui->mdiArea->subWindowList())
    {
        const auto identity = fastecu::ui::parse_map_window_id(window);
        if (identity.has_value() && identity->session == id && identity->map_number == map_index)
        {
            if (auto *map_window = qobject_cast<CalibrationMaps *>(window->widget()); map_window != nullptr)
            {
                map_window->refresh();
            }
        }
    }
}

void MainWindow::checkbox_state_changed(int state)
{
    const auto id = fastecu::ui::parse_map_window_id(ui->mdiArea->activeSubWindow());
    if (id.has_value())
    {
        set_map_switch(id->session, id->map_number, state);
    }
}

void MainWindow::set_map_switch(fastecu::calibration::SessionId id, int map_index, int state)
{
    // No retained definition format populated StateList. RomRaider switches
    // resolve to bloblist Selectable and use set_map_selection. Preserve the
    // empty legacy switch path without adding definition support.
    if (calibrationWorkspace->find(id) == nullptr)
    {
        return;
    }
    (void)map_index;
    (void)state;
}

void MainWindow::calibration_files_treewidget_item_selected(QTreeWidgetItem *item)
{
    QList<QTreeWidgetItem *> itemList;
    QTreeWidgetItem *selectedItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);

    for (int i = 0; i < ui->calibrationFilesTreeWidget->topLevelItemCount(); i++)
    {
        QTreeWidgetItem *item_local = ui->calibrationFilesTreeWidget->topLevelItem(i);
        item_local->setCheckState(0, Qt::Unchecked);
    }

    selectedItem->setCheckState(0, Qt::Checked);

    QString romId = selectedItem->text(1);

    OpenCalibration *open = selected_open_calibration();
    const fastecu::calibration::CalibrationSession *session =
        open != nullptr ? calibrationWorkspace->find(open->id) : nullptr;
    if (session == nullptr)
    {
        return;
    }
    calibrationTreeWidget->buildCalibrationDataTree(ui->calibrationDataTreeWidget, *session, open->view);
    update_protocol_info(
        fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(*session), fastecu::ui::RomInfoRow::FlashMethod));
}

void MainWindow::calibration_data_treewidget_item_selected(QTreeWidgetItem *item)
{
    const QModelIndex index = ui->calibrationDataTreeWidget->selectionModel()->currentIndex();
    QString selectedText = index.data(Qt::DisplayRole).toString();
    QString selectedRom;

    selectedText = item->text(0);

    // A top-level item is a category header; selecting it opens nothing.
    if (ui->calibrationDataTreeWidget->indexOfTopLevelItem(item) > -1)
    {
        return;
    }
    if (ui->calibrationDataTreeWidget->indexOfTopLevelItem(item->parent()) > -1)
    {
        QTreeWidgetItem *selectedFilesTreeItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);
        QTreeWidgetItem *selectedDataTreeItem = item;
        const auto session = session_of(selectedFilesTreeItem);
        OpenCalibration *open = session.has_value() ? open_calibration(*session) : nullptr;
        if (!session.has_value() || open == nullptr)
        {
            return;
        }
        const auto *rom = calibrationWorkspace->find(*session);
        if (rom == nullptr || rom->definition() == nullptr)
        {
            return;
        }
        const auto& maps = rom->definition()->definition.maps;
        int mapIndex = selectedDataTreeItem->text(1).toInt();
        for (int i = 0; i < static_cast<int>(maps.size()); i++)
        {
            const auto& name = maps[static_cast<std::size_t>(i)].name;
            const QString map_name = name.empty() ? QString(" ") : qs(name);
            if (map_name == selectedText && i == mapIndex)
            {
                if (open->view.open_maps.contains(static_cast<std::size_t>(i)))
                {
                    QList<QMdiSubWindow *> list = ui->mdiArea->findChildren<QMdiSubWindow *>();
                    foreach (QMdiSubWindow *w, list)
                    {
                        if (w->objectName().startsWith(fastecu::ui::session_key_text(*session) + "," +
                                                       QString::number(i) + "," +
                                                       qs(maps[static_cast<std::size_t>(i)].name)))
                        {
                            if (w->objectName() == ui->mdiArea->activeSubWindow()->objectName())
                            {
                                ui->mdiArea->removeSubWindow(w);
                                open->view.open_maps.erase(static_cast<std::size_t>(i));
                                item->setCheckState(0, Qt::Unchecked);
                            }
                            else
                            {
                                w->setFocus();
                                item->setCheckState(0, Qt::Checked);
                            }
                        }
                    }
                }
                else
                {
                    const auto shown = fastecu::ui::present_map(*rom, static_cast<std::size_t>(i));
                    if (!shown.has_value())
                    {
                        emit LOG_E("Error decoding calibration map values [" +
                                       QString(fastecu::to_string(shown.error().kind)) +
                                       "]: " + qs(shown.error().detail),
                                   true, true);
                    }
                    open->view.open_maps.insert(static_cast<std::size_t>(i));
                    item->setCheckState(0, Qt::Checked);

                    CalibrationMaps *calibrationMaps =
                        new CalibrationMaps(*calibrationWorkspace, *session, i, ui->mdiArea->contentsRect());
                    QMdiSubWindow *subWindow = ui->mdiArea->addSubWindow(calibrationMaps);
                    if (subWindow)
                    {
                        subWindow->setAttribute(Qt::WA_DeleteOnClose, true);
                        subWindow->setObjectName(calibrationMaps->objectName());
                        subWindow->show();
                        subWindow->adjustSize();
                        subWindow->move(0, 0);
                        // subWindow->setFixedWidth(subWindow->width());
                        // subWindow->setFixedHeight(subWindow->height());

                        connect(calibrationMaps, &CalibrationMaps::selectable_combobox_item_changed, this,
                                [this, id = *session, i](const QString& value) { set_map_selection(id, i, value); });
                        connect(calibrationMaps, &CalibrationMaps::checkbox_state_changed, this,
                                [this, id = *session, i](int state) { set_map_switch(id, i, state); });
                        connect(subWindow, SIGNAL(destroyed(QObject *)), this, SLOT(close_calibration_map(QObject *)));
                    }
                }
            }
        }
    }
}

void MainWindow::calibration_data_treewidget_item_expanded(QTreeWidgetItem *item)
{
    set_category_expanded(item, true);
}

void MainWindow::calibration_data_treewidget_item_collapsed(QTreeWidgetItem *item)
{
    set_category_expanded(item, false);
}

void MainWindow::set_category_expanded(QTreeWidgetItem *item, bool expanded)
{
    const int itemIndex = ui->calibrationDataTreeWidget->indexOfTopLevelItem(item);
    OpenCalibration *open = selected_open_calibration();
    if (itemIndex < 0 || open == nullptr)
    {
        return;
    }
    const QString categoryName = ui->calibrationDataTreeWidget->topLevelItem(itemIndex)->text(0);
    if (expanded)
    {
        open->view.expanded_categories.insert(categoryName);
    }
    else
    {
        open->view.expanded_categories.erase(categoryName);
    }
    if (categoryName == "ROM Info")
    {
        open->view.rom_info_expanded = expanded;
    }
}

void MainWindow::close_calibration()
{
    const QList<QTreeWidgetItem *> selected = ui->calibrationFilesTreeWidget->selectedItems();
    const auto id = selected.isEmpty() ? std::nullopt : session_of(selected.at(0));
    if (!id.has_value())
    {
        return;
    }
    const int romNumber = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selected.at(0));
    const QString romKey = fastecu::ui::session_key_text(*id);

    const QString windowPrefix = romKey + ",";
    for (QMdiSubWindow *w : ui->mdiArea->subWindowList())
    {
        if (w->objectName().startsWith(windowPrefix))
        {
            ui->mdiArea->removeSubWindow(w);
        }
    }
    delete ui->calibrationFilesTreeWidget->takeTopLevelItem(romNumber);
    std::erase_if(calibrations_, [&id](const OpenCalibration& open) { return open.id == *id; });
    std::ignore = calibrationWorkspace->close(*id);

    if (ui->calibrationFilesTreeWidget->topLevelItemCount() > 0)
    {
        for (int i = 0; i < ui->calibrationFilesTreeWidget->topLevelItemCount(); i++)
        {
            ui->calibrationFilesTreeWidget->topLevelItem(i)->setSelected(false);
        }
        int activeRom = 0;
        if (romNumber > 0)
        {
            activeRom = romNumber - 1;
        }
        QTreeWidgetItem *currentItem = ui->calibrationFilesTreeWidget->topLevelItem(activeRom);
        currentItem->setSelected(true);
        const QModelIndex index = ui->calibrationFilesTreeWidget->selectionModel()->currentIndex();
        emit ui->calibrationFilesTreeWidget->clicked(index);
    }
    else
    {
        for (int i = ui->calibrationDataTreeWidget->topLevelItemCount(); i > 0; i--)
        {
            delete ui->calibrationDataTreeWidget->takeTopLevelItem(0);
        }
    }
    // configSession->settings().calibration_files.erase(...);
    // save_settings();
}

void MainWindow::close_calibration_map(QObject *obj)
{
    const QStringList mapWindowString = obj->objectName().split(",");
    const auto id = mapWindowString.isEmpty() ? std::nullopt : fastecu::ui::parse_session_key(mapWindowString.at(0));
    QTreeWidgetItem *romItem = id.has_value() ? files_tree_item(*id) : nullptr;
    OpenCalibration *open = id.has_value() ? open_calibration(*id) : nullptr;
    if (romItem == nullptr || open == nullptr || mapWindowString.size() < 3)
    {
        return; // the ROM was closed before its window
    }
    const QString& mapName = mapWindowString.at(2);

    for (int i = 0; i < ui->calibrationFilesTreeWidget->topLevelItemCount(); i++)
    {
        ui->calibrationFilesTreeWidget->topLevelItem(i)->setSelected(false);
    }
    romItem->setSelected(true);
    const QModelIndex index = ui->calibrationFilesTreeWidget->selectionModel()->currentIndex();
    emit ui->calibrationFilesTreeWidget->clicked(index);

    QTreeWidgetItem *item;
    for (int i = 0; i < ui->calibrationDataTreeWidget->topLevelItemCount(); i++)
    {
        for (int j = 0; j < ui->calibrationDataTreeWidget->topLevelItem(i)->childCount(); j++)
        {
            item = ui->calibrationDataTreeWidget->topLevelItem(i)->child(j);
            if (item->text(0) == mapName)
            {
                item->setCheckState(0, Qt::Unchecked);

                open->view.open_maps.erase(static_cast<std::size_t>(mapWindowString.at(1).toUInt()));
            }
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    close_app();
}

void MainWindow::close_app()
{
    qApp->exit();
}

void MainWindow::change_gauge_values()
{
    change_log_values(0, protocol);
    // if (ecu_init_complete)
    //     save_logger_selection();
}

void MainWindow::change_digital_values()
{
    change_log_values(1, protocol);
}

void MainWindow::change_switch_values()
{
    change_log_values(2, protocol);
}

void MainWindow::update_logboxes(const QString& protocol_arg)
{
    int switchBoxCount = 20;
    int logBoxCount = 12;

    emit LOG_D("Update logboxes with protocol: " + protocol_arg, true, true);

    while (!ui->switchBoxLayout->isEmpty())
    {
        QWidget *wg = ui->switchBoxLayout->takeAt(0)->widget();
        delete wg;
    }
    while (!ui->logBoxLayout->isEmpty())
    {
        QWidget *wg = ui->logBoxLayout->takeAt(0)->widget();
        delete wg;
    }

    const auto key = protocol_arg.toStdString();
    const auto& selection = loggerModel->selection();
    for (std::size_t slot = 0; slot < selection.switch_ids.size(); ++slot)
    {
        const auto& id = selection.switch_ids[slot];
        const auto *item = loggerModel->switch_definition(key, id);
        if (item == nullptr)
        {
            continue;
        }
        const auto name = qs(item->name);
        auto *box = logBoxes->drawLogBoxes("switch", static_cast<int>(slot), switchBoxCount, name, name,
                                           loggerValues.switch_value(key, id));
        box->setAttribute(Qt::WA_TransparentForMouseEvents);
        ui->switchBoxLayout->addWidget(box);
    }
    for (std::size_t slot = 0; slot < selection.lower_panel_ids.size(); ++slot)
    {
        const auto& id = selection.lower_panel_ids[slot];
        const auto *item = loggerModel->parameter(key, id);
        if (item == nullptr)
        {
            continue;
        }
        const auto unit = item->conversions.empty() ? QString{} : qs(item->conversions.front().units);
        auto *box = logBoxes->drawLogBoxes("log", static_cast<int>(slot), logBoxCount, qs(item->name), unit,
                                           loggerValues.parameter_value(key, id));
        box->setAttribute(Qt::WA_TransparentForMouseEvents);
        ui->logBoxLayout->addWidget(box);
    }
}

void MainWindow::update_logbox_values(const QString& protocol_arg)
{
    const auto key = protocol_arg.toStdString();
    const auto& ids = loggerModel->selection().lower_panel_ids;
    // Layout positions can differ from selection slots when IDs are unresolved.
    // Labels keep the original slot in their object name.
    for (std::size_t slot = 0; slot < ids.size(); ++slot)
    {
        const auto *item = loggerModel->parameter(key, ids[slot]);
        if (item == nullptr)
        {
            continue;
        }
        auto *label = ui->centralwidget->findChild<QLabel *>("log_label" + QString::number(slot));
        if (label == nullptr)
        {
            continue;
        }
        const auto unit = item->conversions.empty() ? QString{} : qs(item->conversions.front().units);
        const auto text = loggerValues.parameter_value(key, ids[slot]);
        label->setAlignment(Qt::AlignRight);
        label->setText(text + " <font size=1px color=grey>" + unit + "</font>");
        const auto size = QGuiApplication::primaryScreen()->geometry();
        label->setFont(QFont("Arial", size.width() / 90));
    }
    delay(1);
}

void MainWindow::load_logger_definition()
{
    auto& settings = configSession->settings();
    auto& service = services_.logger_definitions;
    const auto handle =
        service.resolve_definition_handle(settings.romraider_logger_definition_file, settings.selected_log_protocol,
                                          configSession->effective_paths().config_files_directory);
    if (!handle.has_value())
    {
        services_.file_action_events.notice("Logger file: Unable to resolve logger definition file: " +
                                            handle.error().detail);
        return;
    }
    if (settings.romraider_logger_definition_file.empty() && !handle->empty())
    {
        settings.romraider_logger_definition_file = *handle;
        emit LOG_D("Using bundled CDBG logger definition: " + qs(*handle), true, true);
    }
    auto definition = service.load_definition(*handle);
    if (!definition.has_value())
    {
        services_.file_action_events.notice("Logger file: Unable to open logger definition file '" + *handle +
                                            "' for reading: " + definition.error().detail);
        return;
    }
    loggerModel->install_definition(std::move(*definition));
}

void MainWindow::load_logger_selection()
{
    const auto& handle = configSession->effective_paths().logger_file;
    auto& service = services_.logger_definitions;
    const auto stored = service.load_selection(handle, ecuid.toStdString());
    if (!stored.has_value())
    {
        services_.file_action_events.notice("Logger file: Unable to open logger config file '" + handle +
                                            "' for reading");
        return;
    }
    // A successful read clears IDs even if no ECU entry or definition exists.
    loggerModel->set_selection({.protocol = loggerModel->selection().protocol});
    if (stored->has_value())
    {
        loggerModel->set_selection(**stored);
        return;
    }
    if (loggerModel->definition().parameters.empty())
    {
        services_.file_action_events.notice("Logger definition file: No logger definition file selected, returning "
                                            "without initializing log parameters!");
        return;
    }
    const auto selected =
        service.load_or_initialize_selection(handle, ecuid.toStdString(), loggerModel->default_selection());
    if (!selected.has_value())
    {
        services_.file_action_events.notice("Logger file: Unable to open logger config file '" + handle +
                                            "' for reading");
        return;
    }
    loggerModel->set_selection(*selected);
}

void MainWindow::save_logger_selection()
{
    const auto& handle = configSession->effective_paths().logger_file;
    const auto saved =
        services_.logger_definitions.save_selection(handle, ecuid.toStdString(), loggerModel->selection());
    if (!saved.has_value())
    {
        services_.file_action_events.notice("Logger file: Unable to open logger config file '" + handle +
                                            "' for reading");
    }
}

bool MainWindow::event(QEvent *event)
{
    // Events can arrive before the constructor has bound the session.
    if (configSession != nullptr && (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Resize))
    {
        fastecu::config::AppConfig& settings = configSession->settings();
        if (isMaximized())
        {
            settings.window_width = "maximized";
            settings.window_height = "maximized";
        }
        else
        {
            settings.window_width = QString::number(MainWindow::size().width()).toStdString();
            settings.window_height = QString::number(MainWindow::size().height()).toStdString();
        }
        save_settings();
    }

    return QMainWindow::event(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    // QScreen *screen = QGuiApplication::primaryScreen();
    // QRect size = screen->geometry();
    QWidget::resizeEvent(event);

    // QString window_width = QString::number(MainWindow::size().width());
    // QString window_height = QString::number(MainWindow::size().height());

    // emit LOG_D("Screen resize event 2";

    QWidget *w = ui->mdiArea->findChild<QWidget *>("gaugeWindow");
    if (w)
    {
        delete w;
    }
}

void MainWindow::set_status_bar_label(bool serialConnectionState, bool ecuConnectionState, const QString& romId)
{
    QString connectionStatusString;

    QString softwareVersionString = "FastECU";
    if (ecuConnectionState)
    {
        connectionStatusString = "ECU connected";
        status_bar_connection_label->setStyleSheet("QLabel { background-color : green; color : white; color: white;}");
    }
    else if (serialConnectionState)
    {
        connectionStatusString = "ECU not connected";
        status_bar_connection_label->setStyleSheet("QLabel { background-color : yellow; color : white; color: black;}");
    }
    else
    {
        connectionStatusString = "Serial port unavailable";
        status_bar_connection_label->setStyleSheet("QLabel { background-color : red; color : white; color: white;}");
    }
    QString romIdString = "ECU ID: " + romId;
    QString statusBarString = softwareVersionString + " | " + connectionStatusString + " | " + romIdString;
    status_bar_connection_label->setText(statusBarString);
}

void MainWindow::delay(int n)
{
    QTime dieTime = QTime::currentTime().addMSecs(n);
    while (QTime::currentTime() < dieTime)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

void MainWindow::add_new_ecu_definition_file()
{
    QString filename;
    QObject *obj = sender();
    QListWidget *definition_files = obj->parent()->parent()->findChild<QListWidget *>("ecu_definition_files_list");

    QFileDialog openDialog;
    openDialog.setDefaultSuffix("xml");
    filename = QFileDialog::getOpenFileName(this, tr("Select definition file"),
                                            qs(configSession->effective_paths().definition_files_directory),
                                            tr("ECU definition file (*.xml)"));

    if (filename.isEmpty())
    {
        QMessageBox::information(this, tr("ECU definition file"), "No file selected");
    }
    else
    {
        definition_files->addItem(filename);
        configSession->settings().romraider_definition_files.push_back(filename.toStdString());
        save_settings();
    }
}

void MainWindow::remove_ecu_definition_file()
{
    QObject *obj = sender();
    QListWidget *definition_files = obj->parent()->parent()->findChild<QListWidget *>("ecu_definition_files_list");
    QList<QModelIndex> index = definition_files->selectionModel()->selectedIndexes();

    int row = 0;
    for (auto i = static_cast<int>(index.length()) - 1; i >= 0; i--)
    {
        row = index.at(i).row();
        definition_files->model()->removeRow(row);
        std::vector<std::string>& files = configSession->settings().romraider_definition_files;
        if (static_cast<std::size_t>(row) < files.size())
        {
            files.erase(files.begin() + row);
        }
    }
    if (!index.empty())
    {
        save_settings();
    }
}

void MainWindow::add_new_logger_definition_file()
{
}

void MainWindow::remove_logger_definition_file()
{
}

QString MainWindow::parse_message_to_hex(const QByteArray& received)
{
    QString msg;

    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')));
    }

    return msg;
}

bool MainWindow::eventFilter(QObject *target, QEvent *event)
{
    Q_UNUSED(target)
    // Filter all mouse and keyboard events for splashscreen
    if (target == netSplash)
    {
        if ((event->type() == QEvent::MouseButtonPress) || (event->type() == QEvent::MouseButtonDblClick) ||
            (event->type() == QEvent::MouseButtonRelease) || (event->type() == QEvent::KeyPress) ||
            (event->type() == QEvent::KeyRelease))
        {
            return true;
        }
    }

    return false;
}

template <typename FLASH_CLASS> FLASH_CLASS *MainWindow::connect_signals_and_run_module(FLASH_CLASS *object)
{
    // To successfully connect an overloaded signal,
    // one should provide a suitable template parameter to connect()
    QObject::connect<void (FLASH_CLASS::*)(QString)>(object, &FLASH_CLASS::external_logger, this,
                                                     &MainWindow::external_logger);
    QObject::connect<void (FLASH_CLASS::*)(int)>(object, &FLASH_CLASS::external_logger, this,
                                                 &MainWindow::external_logger_set_progressbar_value);

    // If signal is not overloaded, QObject::connect<> template will deduce type automatically
    QObject::connect(object, &FLASH_CLASS::LOG_E, log_channel, &fastecu::ui::LogChannel::LOG_E);
    QObject::connect(object, &FLASH_CLASS::LOG_W, log_channel, &fastecu::ui::LogChannel::LOG_W);
    QObject::connect(object, &FLASH_CLASS::LOG_I, log_channel, &fastecu::ui::LogChannel::LOG_I);
    QObject::connect(object, &FLASH_CLASS::LOG_D, log_channel, &fastecu::ui::LogChannel::LOG_D);

    object->run();
    return object;
}

// External logger slot for string messages
void MainWindow::external_logger(const QString& message)
{
    emit LOG_D(Q_FUNC_INFO, true, false);
    emit LOG_D(" " + message, false, true);
    emit remote_peer->log_window_message(message);
}

// External progress bar slot
void MainWindow::external_logger_set_progressbar_value(int value)
{
    emit LOG_D(Q_FUNC_INFO, true, false);
    emit LOG_D(" " + QString::number(value), false, true);
    emit remote_peer->progress(value);
}

void MainWindow::send_message_to_log_window(const QString& msg)
{
    // EcuOperationsWindow
    QDialog *ecuOperationsWindow = this->findChild<QDialog *>("EcuOperationsWindow");
    if (ecuOperationsWindow)
    {
        // emit LOG_D("Found ecuOperationsWindow", true, true);
        QTextEdit *textEdit = ecuOperationsWindow->findChild<QTextEdit *>("text_edit");
        if (textEdit)
        {
            // emit LOG_D("Found ecuOperationsWindow->textEdit", true, true);
            textEdit->insertPlainText(msg);
            textEdit->ensureCursorVisible();
        }
    }
    QDialog *biuOperationsSubaruWindow = this->findChild<QDialog *>("BiuOperationsSubaruWindow");
    if (biuOperationsSubaruWindow)
    {
        // emit LOG_D("Found biuOperationsSubaruWindow", true, true);
        QTextEdit *textEdit = biuOperationsSubaruWindow->findChild<QTextEdit *>("text_edit");
        if (textEdit)
        {
            // emit LOG_D("Found biuOperationsSubaruWindow->textEdit", true, true);
            textEdit->insertPlainText(msg);
            textEdit->ensureCursorVisible();
        }
    }
    QDialog *dtcOperationsWindow = this->findChild<QDialog *>("DtcOperationsWindow");
    if (dtcOperationsWindow)
    {
        // emit LOG_D("Found dtcOperationsWindow", true, true);
        QTextEdit *textEdit = dtcOperationsWindow->findChild<QTextEdit *>("text_edit");
        if (textEdit)
        {
            // emit LOG_D("Found dtcOperationsWindow->textEdit", true, true);
            textEdit->insertPlainText(msg);
            textEdit->ensureCursorVisible();
        }
    }
    QDialog *dataTerminalWindow = this->findChild<QDialog *>("DataTerminalWindow");
    if (dataTerminalWindow)
    {
        emit LOG_D("Found dataTerminalWindow", true, true);
        QTextEdit *textEdit = dataTerminalWindow->findChild<QTextEdit *>("text_edit");
        if (textEdit)
        {
            emit LOG_D("Found dataTerminalWindow->textEdit", true, true);
            textEdit->insertPlainText(msg);
            textEdit->ensureCursorVisible();
        }
    }
}

void MainWindow::update_vbatt()
{
    if (connection_coordinator_->identifying())
    {
        return;
    }
    const std::optional<unsigned long> reading = connection->battery_millivolts();
    if (!reading.has_value())
    {
        return;
    }
    const unsigned long vBatt = *reading;

    QDialog *ecuOperationsWindow = this->findChild<QDialog *>("EcuOperationsWindow");
    if (ecuOperationsWindow)
    {
        // emit LOG_D("Found ecuOperationsWindow", true, true);
        QLabel *vBattLabel = ecuOperationsWindow->findChild<QLabel *>("vBattLabel");
        if (vBattLabel)
        {
            // emit LOG_D("Found ecuOperationsWindow->vBattLabel", true, true);
            QString vBattText = QString("Battery: %1").arg(static_cast<double>(vBatt) / 1000.0, 0, 'f', 3) + " V";
            vBattLabel->setText(vBattText);
            emit LOG_D(vBattText, true, true);
        }
    }
    QDialog *biuOperationsSubaruWindow = this->findChild<QDialog *>("BiuOperationsSubaruWindow");
    if (biuOperationsSubaruWindow)
    {
        // emit LOG_D("Found biuOperationsSubaruWindow", true, true);
        QLabel *vBattLabel = biuOperationsSubaruWindow->findChild<QLabel *>("vBattLabel");
        if (vBattLabel)
        {
            // emit LOG_D("Found biuOperationsSubaruWindow->vBattLabel", true, true);
            QString vBattText = QString("Battery: %1").arg(static_cast<double>(vBatt) / 1000.0, 0, 'f', 3) + " V";
            vBattLabel->setText(vBattText);
            emit LOG_D(vBattText, true, true);
        }
    }
    QDialog *dtcOperationsWindow = this->findChild<QDialog *>("DtcOperationsWindow");
    if (dtcOperationsWindow)
    {
        // emit LOG_D("Found dtcOperationsWindow", true, true);
        QLabel *vBattLabel = dtcOperationsWindow->findChild<QLabel *>("vBattLabel");
        if (vBattLabel)
        {
            // emit LOG_D("Found dtcOperationsWindow->vBattLabel", true, true);
            QString vBattText = QString("Battery: %1").arg(static_cast<double>(vBatt) / 1000.0, 0, 'f', 3) + " V";
            vBattLabel->setText(vBattText);
            emit LOG_D(vBattText, true, true);
        }
    }
    QDialog *dataTerminalWindow = this->findChild<QDialog *>("DataTerminalWindow");
    if (dataTerminalWindow)
    {
        emit LOG_D("Found dataTerminalWindow", true, true);
        QLabel *vBattLabel = dataTerminalWindow->findChild<QLabel *>("vBattLabel");
        if (vBattLabel)
        {
            emit LOG_D("Found dataTerminalWindow->vBattLabel", true, true);
            QString vBattText = QString("Battery: %1").arg(static_cast<double>(vBatt) / 1000.0, 0, 'f', 3) + " V";
            vBattLabel->setText(vBattText);
            emit LOG_D(vBattText, true, true);
        }
    }
}

void MainWindow::setupLoggingEngine()
{
    loggingEngine = &services_.logging_engine;

    connect(loggingEngine, &fastecu::desktop::logging::LoggingEngine::valuesUpdated, this,
            &MainWindow::handleLoggingValuesUpdated);
    connect(loggingEngine, &fastecu::desktop::logging::LoggingEngine::sessionEnded, this,
            &MainWindow::handleLoggingSessionEnded);
}

void MainWindow::handleLoggingValuesUpdated(const QVector<fastecu::logging::LogSample>& samples)
{
    if (!activeLoggingSnapshot)
    {
        return;
    }
    for (const auto& sample : samples)
    {
        // Re-checked per sample: LOG_E below can reach a slot that ends the session.
        if (!activeLoggingSnapshot)
        {
            return;
        }
        const auto applied = fastecu::desktop::logging::apply_log_sample(*activeLoggingSnapshot, sample, loggerValues);
        if (!applied)
        {
            emit LOG_E(QString::fromStdString(applied.error().detail), true, true);
        }
    }
    update_logbox_values(activeLogValueProtocolFilter);
    log_to_file();
}

void MainWindow::restoreLoggingUiState()
{
    activeLoggingSnapshot.reset();
    logging_state = false;
    log_params_request_started = false;
    ui->actionToggleRealtime->setChecked(false);
}

void MainWindow::handleLoggingSessionEnded(fastecu::desktop::logging::SessionEndReason reason, const QString& message)
{
    restoreLoggingUiState();

    if (reason == fastecu::desktop::logging::SessionEndReason::StoppedByUser)
    {
        return;
    }

    if (reason == fastecu::desktop::logging::SessionEndReason::AdapterDisconnected)
    {
        QMessageBox::warning(this, tr("Logging"), "Logging adapter disconnected: " + message);
    }
    else if (reason == fastecu::desktop::logging::SessionEndReason::HandshakeFailed)
    {
        QMessageBox::warning(this, tr("Logging"), "Unable to start logging: " + message);
    }
    else if (reason == fastecu::desktop::logging::SessionEndReason::RuntimeFailed)
    {
        QMessageBox::warning(this, tr("Logging"), "Logging stopped: " + message);
    }
}
