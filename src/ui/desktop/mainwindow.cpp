#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QProgressBar>
#include <QScopeGuard>
#include <QSplashScreen>
#include <cstddef>
#include <iterator>
#include <utility>
#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/backend/checksum/checksum_selection.h"
#include "src/backend/config/menu_definition.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_operation_request.h"
#include "src/platform/desktop/common/flash/flash_workflow.h"
#include "src/platform/desktop/common/serial/serial_idle.h"
#include "src/ui/desktop/config_fields.h"
#include "src/ui/desktop/menu/menu_builder.h"
#include "src/ui/desktop/flash/operation/flash_operation_controller.h"

using fastecu::config::ProtocolEntry;
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

    QPixmap startUpSplashImage(":/images/startup_splash.jpg");
    int startUpSplashProgressBarValue = 0;

    startUpSplash = std::make_unique<QSplashScreen>(startUpSplashImage);
    QVBoxLayout *startUpSplashLayout = new QVBoxLayout(startUpSplash.get());
    // startUpSplashLayout->setMargin(0);
    startUpSplashLayout->setSpacing(0);
    startUpSplashLayout->setAlignment(Qt::AlignBottom);
    startUpSplashLabel = new QLabel(QString("Starting FastECU..."));
    startUpSplashLabel->setStyleSheet("QLabel { background-color : black; color : white; }");
    startUpSplashLayout->addWidget(startUpSplashLabel);

    startUpSplashProgressBar = new QProgressBar();
    startUpSplashProgressBar->setMinimum(0);
    startUpSplashProgressBar->setMaximum(100);
    startUpSplashProgressBar->setValue(startUpSplashProgressBarValue);
    startUpSplashProgressBar->setFixedHeight(16);
    startUpSplashLayout->addWidget(startUpSplashProgressBar);
    // startUpSplash->setEnabled(false);
    startUpSplash->show();
    // Move splashscreen to the center of the screen
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    startUpSplash->move(screenGeometry.center() - startUpSplash->rect().center());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    setSplashScreenProgress("Reading config files...", 10);
    fileActions = &services_.file_actions;
    configSession = &services_.config;

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
                     [this](int level, QString message)
                     {
                         switch (static_cast<fastecu::LogLevel>(level))
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
                     });
    QObject::connect(&services_.file_action_events, &QtEventSink::noticed, this,
                     [this](QString message) { QMessageBox::warning(this, software_title, message); });

    definitionAuthoringDialog =
        new fastecu::ui::DefinitionAuthoringDialog(*fileActions, *configSession, services_.config_repository, this);
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

    // DesktopComposition initialized the session (provisioning, settings,
    // catalogs, and a valid saved row) before building this window.
    emit LOG_D("Protocols ID: " + qs(configSession->settings().selected_protocol_id) + "/" +
                   QString::number(configSession->vehicles().size()),
               true, true);

    emit LOG_D(qs(selected_vehicle().make), true, true);
    emit LOG_D(protocol_field(selected_vehicle(), &ProtocolEntry::mcu), true, true);
    emit LOG_D(protocol_field(selected_vehicle(), &ProtocolEntry::checksum), true, true);
    emit LOG_D(qs(selected_vehicle().model), true, true);
    emit LOG_D(qs(selected_vehicle().version), true, true);
    emit LOG_D(qs(selected_vehicle().protocol_name), true, true);
    emit LOG_D(protocol_field(selected_vehicle(), &ProtocolEntry::description), true, true);
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

    setSplashScreenProgress("Preparing ROM definitions...", 10);
    if (configSession->settings().romraider_definition_files.empty() &&
        configSession->settings().ecuflash_definition_files_directory.empty())
    {
        QMessageBox::warning(this, tr("Ecu definition file"),
                             "No definition file(s), use 'Settings' in 'Edit' menu to choose file(s)");
    }

    setSplashScreenProgress("Preparing EcuFlash ROM definitions...", 10);
    fileActions->create_ecuflash_def_id_list();

    setSplashScreenProgress("Preparing RomRaider ROM definitions...", 10);
    fileActions->create_romraider_def_id_list();

    if (const QString kernel_dir = qs(configSession->effective_paths().kernel_files_directory);
        QDir(kernel_dir).exists())
    {
        QDir dir(kernel_dir);
        QStringList nameFilter("*.bin");
        QStringList txtFilesAndDirectories = dir.entryList(nameFilter);
        // emit LOG_D(txtFilesAndDirectories;
    }

    setSplashScreenProgress("Setting up menus...", 10);
    QSignalMapper *mapper = nullptr;
    {
        const fastecu::config::ConfigPaths menu_paths = configSession->effective_paths();
        fastecu::Result<fastecu::config::MenuDefinition> menu_definition =
            fastecu::config::load_menu_definition(menu_paths, services_.config_repository);
        if (!menu_definition.has_value())
        {
            // The same modal read_menu_file raised itself (file_actions.cpp:813);
            // 6a-3 routes this through IEventSink instead.
            QMessageBox::warning(this, tr("Ecu menu file"),
                                 QString("Unable to load menu config file '%1'").arg(qs(menu_paths.menu_file)));
            menu_definition = fastecu::config::MenuDefinition{};
        }
        mapper = fastecu::ui::build_menus(*menu_definition, ui->menubar, ui->toolBar, this);
    }
    connect(mapper, SIGNAL(mappedString(QString)), this, SLOT(menu_action_triggered(QString)));

    /*
        for (int i = 0; i < configSession->settings().calibration_files.size(); i++)
        {
            QString filename = qs(configSession->settings().calibration_files.at(i));
            bool result = false;
            //emit LOG_D("Open file" << filename;
            //ecuCalDef[ecuCalDefIndex] = new FileActions::EcuCalDefStructure;
            result = open_calibration_file(filename);
            if (result)
            {
                configSession->settings().calibration_files.erase(configSession->settings().calibration_files.begin() +
       i); fileActions->saveConfigFile(); i--;
            }
        }
        if(ecuCalDefIndex > 0)
        {
            const QModelIndex index = ui->calibrationFilesTreeWidget->selectionModel()->currentIndex();
            emit ui->calibrationFilesTreeWidget->clicked(index);
        }
    */
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

    setSplashScreenProgress("Setting up statusbar...", 10);
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

    setSplashScreenProgress("Preparing up treewidget...", 10);
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

    setSplashScreenProgress("Preparing remote connection...", 10);
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

    setSplashScreenProgress("Setting up toolbar...", 10);
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

    logValues = &fileActions->LogValuesStruct;
    logValues = fileActions->read_logger_definition_file();
    logBoxes = new LogBox();

    if (logValues != nullptr)
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

    if (ecuCalDefIndex > 0)
    {
        const QModelIndex index = ui->calibrationFilesTreeWidget->selectionModel()->currentIndex();
        emit ui->calibrationFilesTreeWidget->clicked(index);
    }

    emit log_transport_list->currentIndexChanged(log_transport_list->currentIndex());

    status_bar_ecu_label->setText(protocol_field(selected_vehicle(), &ProtocolEntry::description) + " ");

    set_flash_arrow_state();

    startUpSplashLabel->setText("Starting FastECU GUI...");
    startUpSplashProgressBarValue = startUpSplashProgressBar->value();
    while (startUpSplashProgressBarValue < 100)
    {
        startUpSplashProgressBar->setValue(startUpSplashProgressBarValue += 1);
        delay(10);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    startUpSplash->close();
    netSplash->deleteLater();
    emit LOG_I("FastECU initialized", true, true);
}

MainWindow::~MainWindow()
{
    connect_done_ = nullptr;
    stop_identification();
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
            qApp->exit(RESTART_CODE);
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

    flash_protocols.append(protocol_field(selected_vehicle(), &ProtocolEntry::flash_transport).split(","));

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

    log_transports_local.append(protocol_field(selected_vehicle(), &ProtocolEntry::log_transport).split(","));

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

const fastecu::config::ResolvedCarModel& MainWindow::selected_vehicle() const
{
    // DesktopComposition initializes the session before building MainWindow,
    // and the session only ever holds a valid row.
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
    emit LOG_D("Selected protocol: " + qs(configSession->settings().selected_protocol_id), true, true);
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

    status_bar_ecu_label->setText(protocol_field(selected_vehicle(), &ProtocolEntry::description) + " ");
}

void MainWindow::select_vehicle()
{
    VehicleSelect vehicleSelect(*configSession);
    const int result = vehicleSelect.exec();
    apply_vehicle_choice(result, vehicleSelect.chosen_row());
    emit LOG_D("Selected protocol: " + qs(configSession->settings().selected_protocol_id), true, true);
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

    status_bar_ecu_label->setText(protocol_field(selected_vehicle(), &ProtocolEntry::description) + " ");
}

void MainWindow::update_protocol_info(int rom_number)
{

    emit LOG_D("Update protocol info by selected ROM with FlashMethod: " +
                   ecuCalDef[rom_number]->RomInfo.at(fileActions->FlashMethod),
               true, true);
    // The last matching row wins, as the legacy scan did; no match changes
    // nothing.
    const std::string flash_method = ecuCalDef[rom_number]->RomInfo.at(fileActions->FlashMethod).toStdString();
    if (const bool info_updated = configSession->select_by_protocol_name(flash_method); info_updated)
    {
        emit LOG_D("Protocol info for selected ROM updated", true, true);
    }
    else
    {
        emit LOG_D("Could not find protocol for selected ROM!", true, true);
    }
    status_bar_ecu_label->setText(protocol_field(selected_vehicle(), &ProtocolEntry::description) + " ");
}

void MainWindow::set_flash_arrow_state()
{
    QList<QMenu *> menus = ui->menubar->findChildren<QMenu *>();
    foreach (QMenu *menu, menus)
    {
        foreach (QAction *action, menu->actions())
        {
            if (action->isSeparator())
            {
            }
            else if (action->menu())
            {
            }
            else
            {
                if (action->text() == "Read from ecu")
                {
                    if (protocol_capability(selected_vehicle(), &ProtocolEntry::read))
                    {
                        action->setEnabled(true);
                    }
                    else
                    {
                        action->setEnabled(false);
                    }
                }
                if (action->text() == "Test write to ecu")
                {
                    if (protocol_capability(selected_vehicle(), &ProtocolEntry::test_write))
                    {
                        action->setEnabled(true);
                    }
                    else
                    {
                        action->setEnabled(false);
                    }
                }
                if (action->text() == "Write to ecu")
                {
                    if (protocol_capability(selected_vehicle(), &ProtocolEntry::write))
                    {
                        action->setEnabled(true);
                    }
                    else
                    {
                        action->setEnabled(false);
                    }
                }
            }
        }
    }
}

void MainWindow::log_transport_changed()
{
    stop_identification();
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
    stop_identification();
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
    stop_identification();
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
    stop_identification();
    set_realtime_state(false);
    toggle_realtime();

    int rom_number = 0;

    QTreeWidgetItem *selectedItem = nullptr;
    int item_count = ui->calibrationFilesTreeWidget->selectedItems().count();

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

        QByteArray fullRomDataTmp;
        // The read branch allocates the next calibration slot before dispatch
        // (update_protocol_info reads it); anything but a successful read
        // releases it again.
        bool release_read_slot = false;

        if (cmd_type == "test_write" || cmd_type == "write")
        {
            if (item_count > 0)
            {
                selectedItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);
                rom_number = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selectedItem);
            }
            else
            {
                QMessageBox::warning(this, tr("Write ROM"), "No file selected!");
                return 0;
            }

            fullRomDataTmp = ecuCalDef[rom_number]->FullRomData;
            if (protocol_field(selected_vehicle(), &ProtocolEntry::checksum) == "n/a")
            {
                QMessageBox msgBox(
                    QMessageBox::Warning, "Checksum warning",
                    "WARNING! There is no checksum module for this ROM!\n"
                    "Be aware that if this ROM need checksum correction it must be done with another software!",
                    QMessageBox::Ok | QMessageBox::Cancel);
                const auto ret = msgBox.exec();

                if (ret == QMessageBox::Cancel)
                {
                    emit LOG_D("Write canceled!", true, true);
                    ecuCalDef[rom_number]->FullRomData = fullRomDataTmp;
                    return 0;
                }
            }
            if (ecuCalDef[rom_number]->RomInfo.at(fileActions->FlashMethod) == "")
            {
                ecuCalDef[rom_number]->RomInfo.replace(fileActions->FlashMethod, qs(selected_vehicle().protocol_name));
                update_protocol_info(rom_number);
            }
            ecuCalDef[rom_number]->FlashMethod = qs(selected_vehicle().protocol_name);
            ecuCalDef[rom_number]->Kernel = QString::fromStdString(fastecu::flash::kernel_path(
                kernel_dir.toStdString(),
                fastecu::config::protocol_field_or_placeholder(selected_vehicle(), &ProtocolEntry::kernel)));
            ecuCalDef[rom_number]->KernelStartAddr = protocol_field(selected_vehicle(), &ProtocolEntry::kernel_addr);
            ecuCalDef[rom_number]->McuType = protocol_field(selected_vehicle(), &ProtocolEntry::mcu);

            if (protocol_field(selected_vehicle(), &ProtocolEntry::checksum) != "n/a")
            {
                runChecksumCorrection(ecuCalDef[rom_number]);
            }
        }
        else
        {
            rom_number = ecuCalDefIndex;
            ecuCalDef[rom_number] = new FileActions::EcuCalDefStructure;
            release_read_slot = true;
            while (ecuCalDef[rom_number]->RomInfo.length() < ecuCalDef[rom_number]->RomInfoStrings.length())
            {
                ecuCalDef[rom_number]->RomInfo.append(" ");
            }
            ecuCalDef[rom_number]->RomInfo.replace(fileActions->FlashMethod, qs(selected_vehicle().protocol_name));
            update_protocol_info(rom_number);
            ecuCalDef[rom_number]->FlashMethod = qs(selected_vehicle().protocol_name);
            ecuCalDef[rom_number]->Kernel = QString::fromStdString(fastecu::flash::kernel_path(
                kernel_dir.toStdString(),
                fastecu::config::protocol_field_or_placeholder(selected_vehicle(), &ProtocolEntry::kernel)));
            ecuCalDef[rom_number]->KernelStartAddr = protocol_field(selected_vehicle(), &ProtocolEntry::kernel_addr);
            ecuCalDef[rom_number]->McuType = protocol_field(selected_vehicle(), &ProtocolEntry::mcu);
        }

        emit LOG_D("Protocol to use: " + qs(selected_vehicle().protocol_name), true, true);

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
            .protocol = selected_vehicle().protocol_name,
            .mcu = ecuCalDef[rom_number]->McuType.toStdString(),
            .kernel_path = ecuCalDef[rom_number]->Kernel.toStdString(),
            .image =
                fastecu::flash::portableImageForOperation(operation, bytes::view(ecuCalDef[rom_number]->FullRomData)),
            .paths = configSession->effective_paths(),
            .display_filename = ecuCalDef[rom_number]->FileName.toStdString(),
        });

        if (outcome.status == fastecu::flash::FlashOperationStatus::Completed)
        {
            if (outcome.read_bytes)
            {
                ecuCalDef[rom_number]->FullRomData = bytes::toQByteArray(bytes::ByteView(*outcome.read_bytes));
            }
            if (outcome.rom_id)
            {
                ecuCalDef[rom_number]->RomId = QString::fromStdString(*outcome.rom_id);
            }
        }

        if (outcome.status == fastecu::flash::FlashOperationStatus::ServiceActionHandled)
        {
            // The old goto skipped the post-operation block entirely.
        }
        else if (cmd_type == "read")
        {
            if (ecuCalDef[ecuCalDefIndex]->FullRomData.length())
            {
                QDateTime dateTime = dateTime.currentDateTime();
                QString dateTimeString = dateTime.toString("yyyy-MM-dd_hh'h'mm'm'ss's'");

                ecuCalDef[ecuCalDefIndex]->FileName = QString::fromStdString(fastecu::flash::read_image_filename(
                    ecuCalDef[ecuCalDefIndex]->RomId.toStdString(), dateTimeString.toStdString()));

                fileActions->open_subaru_rom_file(ecuCalDef[ecuCalDefIndex], ecuCalDef[ecuCalDefIndex]->FileName);
                update_protocol_info(ecuCalDefIndex);
                if (!ecuCalDef[ecuCalDefIndex]->use_romraider_definition &&
                    !ecuCalDef[ecuCalDefIndex]->use_ecuflash_definition)
                {
                    prompt_for_missing_definition(ecuCalDef[ecuCalDefIndex]);
                }

                calibrationTreeWidget->buildCalibrationFilesTree(ecuCalDefIndex, ui->calibrationFilesTreeWidget,
                                                                 ecuCalDef[ecuCalDefIndex]);
                calibrationTreeWidget->buildCalibrationDataTree(ui->calibrationDataTreeWidget,
                                                                ecuCalDef[ecuCalDefIndex]);

                release_read_slot = false;
                ecuCalDefIndex++;
                save_calibration_file_as();
            }
        }
        else
        {
            ecuCalDef[rom_number]->FullRomData = fullRomDataTmp;
        }

        if (release_read_slot)
        {
            delete ecuCalDef[rom_number];
            ecuCalDef[rom_number] = nullptr;
        }
    }
    return 0;
}

void MainWindow::custom_menu_requested(QPoint pos)
{
    if (ecuCalDefIndex == 0)
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

    ecuCalDef[ecuCalDefIndex] = new FileActions::EcuCalDefStructure;

    while (ecuCalDef[ecuCalDefIndex]->RomInfo.length() < ecuCalDef[ecuCalDefIndex]->RomInfoStrings.length())
    {
        ecuCalDef[ecuCalDefIndex]->RomInfo.append(" ");
    }

    ecuCalDef[ecuCalDefIndex] = fileActions->open_subaru_rom_file(ecuCalDef[ecuCalDefIndex], std::move(filename));
    if (ecuCalDef[ecuCalDefIndex] != nullptr)
    {
        update_protocol_info(ecuCalDefIndex);
        if (!ecuCalDef[ecuCalDefIndex]->use_romraider_definition && !ecuCalDef[ecuCalDefIndex]->use_ecuflash_definition)
        {
            prompt_for_missing_definition(ecuCalDef[ecuCalDefIndex]);
        }

        calibrationTreeWidget->buildCalibrationFilesTree(ecuCalDefIndex, ui->calibrationFilesTreeWidget,
                                                         ecuCalDef[ecuCalDefIndex]);
        calibrationTreeWidget->buildCalibrationDataTree(ui->calibrationDataTreeWidget, ecuCalDef[ecuCalDefIndex]);

        ecuCalDefIndex++;
    }
    else
    {
        // emit LOG_D("Failed!!";
        ecuCalDef[ecuCalDefIndex] = {};
        return 1;
    }
    return 0;
}

void MainWindow::prompt_for_missing_definition(FileActions::EcuCalDefStructure *ecuCalDef)
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
            definitionAuthoringDialog->create_new_definition(ecuCalDef);
        }
        else if (useExistingRadioButton->isChecked())
        {
            emit LOG_D(useExistingRadioButton->text(), true, true);
            definitionAuthoringDialog->use_existing_definition(ecuCalDef);
        }
    }
    // The "continue without definition" placeholders belong to this branch
    // only -- a user who picked create-new or use-existing must not get them
    // stamped over the definition they just provided.
    if (continueWithoutRadioButton->isChecked() || result == QDialog::Rejected)
    {
        emit LOG_D(continueWithoutRadioButton->text(), true, true);
        fileActions->apply_missing_definition_defaults(ecuCalDef);
    }
}

// Replaces FileActions::checksum_correction, which was deleted in step 5e so
// that no QMessageBox is raised from src/backend. The dialog sequence lives in
// ChecksumCorrectionCommand; the log lines stay here, emitted through
// MainWindow's own LOG_D/LOG_E signals (same signature FileActions used, so the
// text carries over verbatim).
void MainWindow::runChecksumCorrection(FileActions::EcuCalDefStructure *ecuCalDef)
{
    const fastecu::checksum::ChecksumSelection selection{
        .make = selected_vehicle().make,
        .checksum_flag = fastecu::config::protocol_field_or_placeholder(selected_vehicle(), &ProtocolEntry::checksum),
        .flash_method = selected_vehicle().protocol_name,
        .mcu_type = ecuCalDef->McuType.toStdString(),
        .rom_id = ecuCalDef->RomId.toStdString(),
    };

    emit LOG_D("Protocol: " + qs(selection.flash_method), true, true);
    emit LOG_D("Make: " + qs(selection.make), true, true);
    emit LOG_D("Checksum: " + qs(selection.checksum_flag), true, true);

    // The command runs this same lookup as its own precheck; it is repeated
    // here only because the two known-MCU-path log lines below need the
    // flashdev_t. The unknown case is reported back via unknown_mcu_type.
    const flashdev_t *device = fastecu::flash::find_flash_device(selection.mcu_type);
    if (device != nullptr)
    {
        emit LOG_D("ecuCalDef->McuType: " + ecuCalDef->McuType + " " +
                       protocol_field(selected_vehicle(), &ProtocolEntry::mcu),
                   true, true);
        emit LOG_D("Size: 0x" + QString::number(ecuCalDef->FullRomData.length(), 16) + " -> 0x" +
                       QString::number(device->romsize, 16),
                   true, true);
    }

    const fastecu::ui::ChecksumCorrectionResult result =
        m_checksumCorrectionCommand.run(bytes::view(ecuCalDef->FullRomData), ecuCalDef->use_romraider_definition,
                                        ecuCalDef->use_ecuflash_definition, selection, this);

    if (result.unknown_mcu_type)
    {
        emit LOG_E("Unknown MCU type: " + ecuCalDef->McuType, true, true);
        return;
    }
    if (result.canceled_due_to_missing_module)
    {
        emit LOG_D("Checksum calculation canceled!", true, true);
    }
    if (result.corrected_rom_data.has_value())
    {
        ecuCalDef->FullRomData = bytes::toQByteArray(bytes::ByteView(*result.corrected_rom_data));
    }
}

void MainWindow::save_calibration_file()
{
    if (ecuCalDefIndex == 0)
    {
        QMessageBox::information(this, tr("Calibration file"), "No calibration to save!");
        return;
    }

    QTreeWidgetItem *selectedItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);

    int rom_number = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selectedItem);

    QByteArray fullRomDataTmp = ecuCalDef[rom_number]->FullRomData;

    // update_protocol_info(rom_number);
    runChecksumCorrection(ecuCalDef[rom_number]);

    if (ecuCalDef[rom_number] != nullptr)
    {
        // save_subaru_rom_file returns nullptr when the write failed (it has
        // already told the user so); do not follow that with the log lines
        // that read as a successful save.
        if (fileActions->save_subaru_rom_file(ecuCalDef[rom_number], ecuCalDef[rom_number]->FullFileName) != nullptr)
        {
            emit LOG_D("ecuCalDef->FileName: " + ecuCalDef[rom_number]->FileName, true, true);
            emit LOG_D("ecuCalDef->FullFileName: " + ecuCalDef[rom_number]->FullFileName, true, true);
        }
        else
        {
            emit LOG_E("Calibration file not saved: " + ecuCalDef[rom_number]->FullFileName, true, true);
        }
    }

    ecuCalDef[rom_number]->FullRomData = fullRomDataTmp;
}

void MainWindow::save_calibration_file_as()
{
    if (ecuCalDefIndex == 0)
    {
        QMessageBox::information(this, tr("Calibration file"), "No calibration to save!");
        return;
    }

    QTreeWidgetItem *selectedItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);

    emit LOG_D("Save as: Check selected ROM number", true, true);
    int rom_number = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selectedItem);

    QByteArray fullRomDataTmp = ecuCalDef[rom_number]->FullRomData;

    // update_protocol_info(rom_number);
    runChecksumCorrection(ecuCalDef[rom_number]);

    if (ecuCalDef[rom_number] != nullptr)
    {
        QString filename = ui->calibrationFilesTreeWidget->selectedItems().at(0)->text(0);
        // QString filename = "";

        QFileDialog saveDialog;
        saveDialog.setDefaultSuffix("bin");
        emit LOG_D("Save as: Check if OEM ECU file", true, true);

        filename =
            QFileDialog::getSaveFileName(this, tr("Save calibration file"),
                                         qs(configSession->effective_paths().calibration_files_directory) + filename,
                                         tr("Calibration file (*.bin)"));

        if (filename.isEmpty())
        {
            // ecuCalDef[rom_number]->FileName = "No name.bin";
            ui->calibrationFilesTreeWidget->selectedItems().at(0)->setText(0, ecuCalDef[rom_number]->FileName);
            QMessageBox::information(this, tr("Calibration file"), "No file name selected");
            ecuCalDef[rom_number]->FullRomData = fullRomDataTmp;
            return;
        }

        if (filename.endsWith(QString(".")))
        {
            filename.remove(filename.length() - 1, 1);
        }
        if (!filename.endsWith(QString(".bin")))
        {
            filename.append(QString(".bin"));
        }

        // A nullptr return means the write failed (the warning dialog for it
        // was already raised inside save_subaru_rom_file); leave the tree item
        // showing the name the ROM still has on disk and log the failure
        // instead of the two "saved as" lines.
        if (fileActions->save_subaru_rom_file(ecuCalDef[rom_number], filename) != nullptr)
        {
            ui->calibrationFilesTreeWidget->selectedItems().at(0)->setText(0, ecuCalDef[rom_number]->FileName);
            emit LOG_D("ecuCalDef->FileName: " + ecuCalDef[rom_number]->FileName, true, true);
            emit LOG_D("ecuCalDef->FullFileName: " + ecuCalDef[rom_number]->FullFileName, true, true);
        }
        else
        {
            emit LOG_E("Calibration file not saved: " + filename, true, true);
        }
    }
    ecuCalDef[rom_number]->FullRomData = fullRomDataTmp;
}

void MainWindow::selectable_combobox_item_changed(const QString& item)
{
    int mapRomNumber = 0;
    int mapNumber = 0;
    bool bStatus = false;

    // bool ok = false;

    QMdiSubWindow *w = ui->mdiArea->activeSubWindow();
    if (w)
    {
        QStringList mapWindowString = w->objectName().split(",");
        mapRomNumber = mapWindowString.at(0).toInt();
        mapNumber = mapWindowString.at(1).toInt();

        QTableWidget *mapTableWidget = w->findChild<QTableWidget *>(w->objectName());
        if (mapTableWidget)
        {
            QStringList selectionsNameList = ecuCalDef[mapRomNumber]->SelectionsNameList.at(mapNumber).split(",");
            QStringList selectionsValueList = ecuCalDef[mapRomNumber]->SelectionsValueList.at(mapNumber).split(",");

            for (int j = 0; j < selectionsNameList.length(); j++)
            {
                if (selectionsNameList.at(j) == item)
                {
                    // emit LOG_D("Old selectable value was: " + ecuCalDef[mapRomNumber]->MapData.at(mapNumber), true,
                    // true);
                    ecuCalDef[mapRomNumber]->MapData.replace(mapNumber, selectionsValueList.at(j));
                    // emit LOG_D("New selectable value is: " + ecuCalDef[mapRomNumber]->MapData.at(mapNumber), true,
                    // true);

                    if (ecuCalDef[mapRomNumber]->StorageTypeList.at(mapNumber) == "bloblist")
                    {
                        uint8_t storagesize = 0;
                        uint8_t dataByte = 0;
                        uint32_t byteAddress = ecuCalDef[mapRomNumber]->AddressList.at(mapNumber).toUInt(&bStatus, 16);
                        storagesize =
                            ecuCalDef[mapRomNumber]->SelectionsValueList.at(mapNumber).split(",").at(0).length() / 2;
                        for (int k = 0; k < storagesize; k++)
                        {
                            dataByte = ecuCalDef[mapRomNumber]
                                           ->MapData.at(mapNumber)
                                           .mid(static_cast<qsizetype>(k * 2), 2)
                                           .toUInt(&bStatus, 16);
                            ecuCalDef[mapRomNumber]->FullRomData[byteAddress + k] = dataByte;
                        }
                    }
                }
            }
        }
    }
}

void MainWindow::checkbox_state_changed(int state)
{

    bool bStatus;

    int map_rom_number = 0;
    int map_number = 0;

    QMdiSubWindow *w = ui->mdiArea->activeSubWindow();
    if (w)
    {
        QStringList mapWindowString = w->objectName().split(",");
        map_rom_number = mapWindowString.at(0).toInt();
        map_number = mapWindowString.at(1).toInt();

        QTableWidget *mapTableWidget = w->findChild<QTableWidget *>(w->objectName());
        if (mapTableWidget)
        {
            QStringList switch_states = ecuCalDef[map_rom_number]->StateList.at(map_number).split(",");
            int switch_states_length = (switch_states.length() - 1);
            for (int i = 0; i < switch_states_length; i += 2)
            {
                QStringList switch_data = switch_states.at(i + 1).split(" ");
                uint32_t byte_address = ecuCalDef[map_rom_number]->AddressList.at(map_number).toUInt(&bStatus, 16);
                if (ecuCalDef[map_rom_number]->RomInfo.at(fileActions->FlashMethod) == "wrx02" &&
                    ecuCalDef[map_rom_number]->FileSize.toUInt() < (170 * 1024) && byte_address > 0x27FFF)
                {
                    byte_address -= 0x8000;
                }
                if ((switch_states.at(i) == "off" || switch_states.at(i) == "disabled") && state == 0)
                {
                    for (int j = 0; j < switch_data.length(); j++)
                    {
                        emit LOG_D("Old switch value " + QString::number(j) +
                                       " is: " + ecuCalDef[map_rom_number]->FullRomData[byte_address + j],
                                   true, true);
                        ecuCalDef[map_rom_number]->FullRomData[byte_address + j] = (uint8_t)switch_data.at(j).toUInt();
                        emit LOG_D("New switch value " + QString::number(j) +
                                       " is: " + ecuCalDef[map_rom_number]->FullRomData[byte_address + j],
                                   true, true);
                    }
                }
                if ((switch_states.at(i) == "on" || switch_states.at(i) == "enabled") && state == 2)
                {
                    for (int j = 0; j < switch_data.length(); j++)
                    {
                        emit LOG_D("Old switch value" + QString::number(j) +
                                       " is: " + ecuCalDef[map_rom_number]->FullRomData[byte_address + j],
                                   true, true);
                        ecuCalDef[map_rom_number]->FullRomData[byte_address + j] = (uint8_t)switch_data.at(j).toUInt();
                        emit LOG_D("New switch value" + QString::number(j) +
                                       " is: " + ecuCalDef[map_rom_number]->FullRomData[byte_address + j],
                                   true, true);
                    }
                }
            }
        }
    }
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

    int rom_number = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selectedItem);

    selectedItem->setCheckState(0, Qt::Checked);

    QString romId = selectedItem->text(1);

    calibrationTreeWidget->buildCalibrationDataTree(ui->calibrationDataTreeWidget, ecuCalDef[rom_number]);
    update_protocol_info(rom_number);
    /*
        QComboBox *flash_method_list = ui->toolBar->findChild<QComboBox*>("flash_method_list");
        for (int i = 0; i < flash_method_list->count(); i++)
        {
            if(ecuCalDef[romNumber]->RomInfo.at(fileActions->FlashMethod) == flash_method_list->itemText(i))
            {
                flash_method_list->setCurrentIndex(i);
            }
        }
        */
}

void MainWindow::calibration_data_treewidget_item_selected(QTreeWidgetItem *item)
{
    const QModelIndex index = ui->calibrationDataTreeWidget->selectionModel()->currentIndex();
    QString selectedText = index.data(Qt::DisplayRole).toString();
    int hierarchyLevel = 1;
    QModelIndex seekRoot = index;
    QString selectedRom;

    selectedText = item->text(0);

    while (seekRoot.parent() != QModelIndex())
    {
        seekRoot = seekRoot.parent();
        hierarchyLevel++;
    }

    if (ui->calibrationDataTreeWidget->indexOfTopLevelItem(item) > -1)
    {
        hierarchyLevel = 1;
    }
    else if (ui->calibrationDataTreeWidget->indexOfTopLevelItem(item->parent()) > -1)
    {
        hierarchyLevel = 2;

        QTreeWidgetItem *selectedFilesTreeItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);
        QTreeWidgetItem *selectedDataTreeItem = item;
        int romNumber = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selectedFilesTreeItem);
        int mapIndex = selectedDataTreeItem->text(1).toInt();
        int romIndex = selectedFilesTreeItem->text(2).toInt();
        for (int i = 0; i < ecuCalDef[romNumber]->NameList.count(); i++)
        {
            if (ecuCalDef[romNumber]->NameList.at(i) == selectedText && i == mapIndex)
            {
                if (ecuCalDef[romNumber]->VisibleList.at(i) == "1")
                {
                    int map_index = 0;
                    QList<QMdiSubWindow *> list = ui->mdiArea->findChildren<QMdiSubWindow *>();
                    foreach (QMdiSubWindow *w, list)
                    {
                        map_index++;
                        if (w->objectName().startsWith(QString::number(romIndex) + "," + QString::number(i) + "," +
                                                       ecuCalDef[romNumber]->NameList.at(i)))
                        {
                            if (w->objectName() == ui->mdiArea->activeSubWindow()->objectName())
                            {
                                ui->mdiArea->removeSubWindow(w);
                                ecuCalDef[romNumber]->VisibleList.replace(i, "0");
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
                    ecuCalDef[romNumber]->VisibleList.replace(i, "1");
                    item->setCheckState(0, Qt::Checked);

                    CalibrationMaps *calibrationMaps =
                        new CalibrationMaps(ecuCalDef[romNumber], romIndex, i, ui->mdiArea->contentsRect());
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

                        connect(calibrationMaps, SIGNAL(selectable_combobox_item_changed(QString)), this,
                                SLOT(selectable_combobox_item_changed(QString)));
                        connect(calibrationMaps, SIGNAL(checkbox_state_changed(int)), this,
                                SLOT(checkbox_state_changed(int)));
                        connect(subWindow, SIGNAL(destroyed(QObject *)), this, SLOT(close_calibration_map(QObject *)));
                    }
                }
            }
        }
    }
}

void MainWindow::calibration_data_treewidget_item_expanded(QTreeWidgetItem *item)
{
    QTreeWidgetItem *selectedItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);

    int itemIndex = ui->calibrationDataTreeWidget->indexOfTopLevelItem(item);
    int romNumber = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selectedItem);
    QString categoryName = ui->calibrationDataTreeWidget->topLevelItem(itemIndex)->text(0);

    calibrationTreeWidget->calibrationDataTreeWidgetItemExpanded(ecuCalDef[romNumber], categoryName);
}

void MainWindow::calibration_data_treewidget_item_collapsed(QTreeWidgetItem *item)
{
    QTreeWidgetItem *selectedItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);

    int itemIndex = ui->calibrationDataTreeWidget->indexOfTopLevelItem(item);
    int romNumber = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selectedItem);
    QString categoryName = ui->calibrationDataTreeWidget->topLevelItem(itemIndex)->text(0);

    calibrationTreeWidget->calibrationDataTreeWidgetItemCollapsed(ecuCalDef[romNumber], categoryName);
}

void MainWindow::close_calibration()
{
    QTreeWidgetItem *selectedItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);

    int romNumber = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selectedItem);
    int romIndex = ui->calibrationFilesTreeWidget->selectedItems().at(0)->text(2).toInt();

    for (int i = 0; i < ui->calibrationDataTreeWidget->topLevelItemCount(); i++)
    {
        for (int j = 0; j < ui->calibrationDataTreeWidget->topLevelItem(i)->childCount(); j++)
        {
            for (int k = 0; k < ui->mdiArea->subWindowList().count(); k++)
            {
                int mapNumber = ui->calibrationDataTreeWidget->topLevelItem(i)->child(j)->text(1).toInt();
                QString mapName = ui->calibrationDataTreeWidget->topLevelItem(i)->child(j)->text(0);
                QString mapType = ecuCalDef[romNumber]->TypeList.at(mapNumber);

                QString calMapWindowName = QString::number(romIndex) + "," + QString::number(mapNumber) + "," + mapName;
                QMdiSubWindow *w = ui->mdiArea->subWindowList().at(k);
                if (w->objectName().startsWith(calMapWindowName))
                {
                    ui->mdiArea->removeSubWindow(w);
                }
            }
        }
    }
    delete ui->calibrationFilesTreeWidget->takeTopLevelItem(romNumber);

    ecuCalDefIndex--;
    for (int i = romNumber; i < ecuCalDefIndex; i++)
    {
        if (ecuCalDefIndex > romNumber)
        {
            ecuCalDef[i] = ecuCalDef[i + 1];
            int rom_index = ui->calibrationFilesTreeWidget->topLevelItem(i)->text(2).toInt();
            ui->calibrationFilesTreeWidget->topLevelItem(i)->setText(2, QString::number(rom_index - 1));
        }
    }
    ecuCalDef[ecuCalDefIndex] = {};

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
    QStringList mapWindowString = obj->objectName().split(",");
    int mapRomNumber = mapWindowString.at(0).toInt();
    const QString& mapName = mapWindowString.at(2);

    for (int i = 0; i < ui->calibrationFilesTreeWidget->topLevelItemCount(); i++)
    {
        ui->calibrationFilesTreeWidget->topLevelItem(i)->setSelected(false);
    }
    QTreeWidgetItem *currentItem = ui->calibrationFilesTreeWidget->topLevelItem(mapRomNumber);
    currentItem->setSelected(true);
    const QModelIndex index = ui->calibrationFilesTreeWidget->selectionModel()->currentIndex();
    emit ui->calibrationFilesTreeWidget->clicked(index);

    QTreeWidgetItem *selectedItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);
    int romIndex = selectedItem->text(2).toInt();

    QTreeWidgetItem *item;
    for (int i = 0; i < ui->calibrationDataTreeWidget->topLevelItemCount(); i++)
    {
        for (int j = 0; j < ui->calibrationDataTreeWidget->topLevelItem(i)->childCount(); j++)
        {
            item = ui->calibrationDataTreeWidget->topLevelItem(i)->child(j);
            if (item->text(0) == mapName)
            {
                if (mapRomNumber == romIndex)
                {
                    item->setCheckState(0, Qt::Unchecked);
                }

                for (int i_local = 0; i_local < ecuCalDef[mapRomNumber]->NameList.count(); i_local++)
                {
                    if (ecuCalDef[mapRomNumber]->NameList.at(i_local) == mapName)
                    {
                        ecuCalDef[mapRomNumber]->VisibleList.replace(i_local, "0");
                    }
                }
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
    //     fileActions->read_logger_conf(logValues, ecuid, true);
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

    for (int i = 0; i < logValues->lower_panel_switch_id.count(); i++)
    {
        for (int j = 0; j < logValues->log_switch_id.length(); j++)
        {
            if (logValues->lower_panel_switch_id.at(i) == logValues->log_switch_id.at(j) &&
                logValues->log_switch_protocol.at(j) == protocol_arg)
            {
                // emit LOG_D("Switch:" << logValues->log_switch_name.at(j);
                QGroupBox *switchBox =
                    logBoxes->drawLogBoxes("switch", i, switchBoxCount, logValues->log_switch_name.at(j),
                                           logValues->log_switch_name.at(j), logValues->log_switch_state.at(j));
                switchBox->setAttribute(Qt::WA_TransparentForMouseEvents);
                ui->switchBoxLayout->addWidget(switchBox);
            }
        }
    }
    for (int i = 0; i < logValues->lower_panel_log_value_id.count(); i++)
    {
        for (int j = 0; j < logValues->log_value_id.length(); j++)
        {
            if (logValues->lower_panel_log_value_id.at(i) == logValues->log_value_id.at(j) &&
                logValues->log_value_protocol.at(j) == protocol_arg)
            {
                // emit LOG_D("Log value:" << logValues->log_value_name.at(j);
                const auto& conversions = logValues->log_value_conversions.at(j);
                const QString unit =
                    conversions.isEmpty() ? QString() : QString::fromStdString(conversions.at(0).units);
                QGroupBox *logBox = logBoxes->drawLogBoxes("log", i, logBoxCount, logValues->log_value_name.at(j), unit,
                                                           logValues->log_value.at(j));
                logBox->setAttribute(Qt::WA_TransparentForMouseEvents);
                ui->logBoxLayout->addWidget(logBox);
            }
        }
    }
}

void MainWindow::update_logbox_values(const QString& protocol_arg)
{
    int index = 0;
    QString warningMin;
    QString warningMax;
    QString labelText;
    QString label;
    QString unit;

    QScreen *screen = QGuiApplication::primaryScreen();
    QRect size = screen->geometry();

    for (int i = 0; i < logValues->lower_panel_log_value_id.length(); i++)
    {
        QWidget *wg = ui->logBoxLayout->itemAt(i)->widget();
        QVBoxLayout *logVBoxLayout = wg->findChild<QVBoxLayout *>();
        if (logVBoxLayout)
        {
            QLabel *log_label = wg->findChild<QLabel *>("log_label" + QString::number(i));
            if (log_label)
            {

                for (int j = 0; j < logValues->log_value_id.count(); j++)
                {
                    if (logValues->log_value_id.at(j) == logValues->lower_panel_log_value_id.at(i) &&
                        logValues->log_value_protocol.at(j) == protocol_arg)
                    {
                        index = j;
                    }
                }

                const auto& conversions = logValues->log_value_conversions.at(index);
                unit = conversions.isEmpty() ? QString() : QString::fromStdString(conversions.at(0).units);

                labelText = logValues->log_value.at(index);
                labelText.append(" <font size=1px color=grey>");
                labelText.append(unit);
                labelText.append("</font>");

                log_label->setAlignment(Qt::AlignRight);
                log_label->setText(labelText);
                int labelFontSize = size.width() / 90;
                QFont f("Arial", labelFontSize);
                log_label->setFont(f);
            }
        }
    }
    delay(1);
}

void MainWindow::setSplashScreenProgress(const QString& text, int incValue)
{
    startUpSplashLabel->setText(text);
    int startUpSplashProgressBarValue = startUpSplashProgressBar->value();
    startUpSplashProgressBar->setValue(startUpSplashProgressBarValue += incValue);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
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
    for (int i = index.length() - 1; i >= 0; i--)
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
    QObject *obj = sender();
}

void MainWindow::remove_logger_definition_file()
{
    QObject *obj = sender();
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
    if (identify_worker_)
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
            QString vBattText = QString("Battery: %1").arg(vBatt / 1000.0, 0, 'f', 3) + " V";
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
            QString vBattText = QString("Battery: %1").arg(vBatt / 1000.0, 0, 'f', 3) + " V";
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
            QString vBattText = QString("Battery: %1").arg(vBatt / 1000.0, 0, 'f', 3) + " V";
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
            QString vBattText = QString("Battery: %1").arg(vBatt / 1000.0, 0, 'f', 3) + " V";
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
        const auto applied = fastecu::desktop::logging::apply_log_sample(*activeLoggingSnapshot, sample, *logValues);
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
    QList<QMenu *> menus = ui->menubar->findChildren<QMenu *>();
    foreach (QMenu *menu, menus)
    {
        foreach (QAction *action, menu->actions())
        {
            if (action->text() == "Logging")
            {
                action->setChecked(false);
            }
        }
    }
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
