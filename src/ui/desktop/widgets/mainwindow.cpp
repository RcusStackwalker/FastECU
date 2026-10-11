#include "src/ui/desktop/widgets/mainwindow.h"
#include "src/backend/calibration/session/selectable_edit_use_case.h"
#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"
#include "src/ui/desktop/calibration/map_presentation.h"
#include "src/ui/desktop/calibration/map_edit_adapter.h"
#include "src/ui/desktop/calibration/qt_calibration_interaction.h"
#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"
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
using fastecu::ui::checksumField;
using fastecu::ui::kernelAddressField;
using fastecu::ui::protocolCapability;
using fastecu::ui::protocolField;
using fastecu::ui::qs;

const QColor MainWindow::kRedLightOff = QColor(96, 32, 32);
const QColor MainWindow::kYellowLightOff = QColor(96, 96, 32);
const QColor MainWindow::kGreenLightOff = QColor(32, 96, 32);
const QColor MainWindow::kRedLightOn = QColor(255, 64, 64);
const QColor MainWindow::kYellowLightOn = QColor(223, 223, 64);
const QColor MainWindow::kGreenLightOn = QColor(64, 255, 64);

MainWindow::MainWindow(MainWindowServices services, const QString& peerAddress, QWidget *parent)
    : QMainWindow(parent), services_(services), peer_address_(peerAddress), ui_{std::make_unique<Ui::MainWindow>()}
{
    ui_->setupUi(this);
    qApp->installEventFilter(this);
    qRegisterMetaType<QVector<int>>("QVector<int>");

    int id = QFontDatabase::addApplicationFont(":/fonts/FastECU_bars.ttf");
    if (id >= 0)
    {
        QString family = QFontDatabase::applicationFontFamilies(id).at(0);
        QFont monospace(family);
    }

    config_session_ = &services_.config;
    calibration_workspace_ = &services_.calibrations;

    software_name_ = qs(services_.application.name);
    software_title_ = qs(services_.application.title);
    software_version_ = qs(services_.application.version);
    this->setWindowTitle(software_title_ + " " + software_version_);

    log_channel_ = &services_.log;
    using fastecu::ui::LogChannel;
    QObject::connect(this, &MainWindow::logE, log_channel_, &LogChannel::logE);
    QObject::connect(this, &MainWindow::logW, log_channel_, &LogChannel::logW);
    QObject::connect(this, &MainWindow::logI, log_channel_, &LogChannel::logI);
    QObject::connect(this, &MainWindow::logD, log_channel_, &LogChannel::logD);
    QObject::connect(this, &MainWindow::enableLogWriteToFile, log_channel_, &LogChannel::enableLogWriteToFile);
    QObject::connect(log_channel_, &LogChannel::logWindowMessage, this, &MainWindow::sendMessageToLogWindow);

    setupLoggingEngine();

#if defined Q_OS_UNIX
    emit logD("Running on Linux Desktop ", true, false);
    // serialPort = serialPortLinux;
    serial_port_prefix_ = "/dev/";
#elif defined Q_OS_WIN32
    emit logD("Running on Windows Desktop ", true, false);
    // serialPort = serialPortWindows;
    serial_port_prefix_ = "";
#endif

#if Q_PROCESSOR_WORDSIZE == 4
    emit logD("32-bit executable", true, true);
#elif Q_PROCESSOR_WORDSIZE == 8
    emit logD("64-bit executable", true, true);
#endif

    QObject::connect(&services_.file_action_events, &QtEventSink::logged, this,
                     [this](int level, const QString& message)
                     { emitLogLine(static_cast<fastecu::LogLevel>(level), message); });
    QObject::connect(&services_.file_action_events, &QtEventSink::noticed, this,
                     [this](QString message) { QMessageBox::warning(this, software_title_, message); });

    calibration_interaction_ = std::make_unique<fastecu::ui::QtCalibrationInteraction>(this);
    calibration_operations_ = std::make_unique<fastecu::ui::CalibrationOperationCoordinator>(
        *config_session_, services_.rom_save, *calibration_interaction_,
        fastecu::ui::CalibrationPresentationCallbacks{
            .log = [this](fastecu::LogLevel level, std::string_view text)
            { emitLogLine(level, QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()))); },
            .protocol_description_changed =
                [this](std::string_view description)
            {
                status_bar_ecu_label_->setText(
                    QString::fromUtf8(description.data(), static_cast<qsizetype>(description.size())) + " ");
            },
        });

    identify_launcher_ = std::make_unique<fastecu::ui::QtIdentifyLauncher>(
        [this] { return std::make_unique<fastecu::diagnostics::SerialDiagnosticLink>(&connection_->Facade()); },
        services_.make_clock,
        [this](fastecu::LogLevel level, const QString& message)
        {
            if (level == fastecu::LogLevel::kWarning)
            {
                emit logW(message, true, true);
            }
            else
            {
                emit logD(message, true, true);
            }
        });
    connection_coordinator_ =
        std::make_unique<fastecu::ui::ConnectionCoordinator>(*identify_launcher_, connection_presentation_);

    definition_authoring_dialog_ = new fastecu::ui::DefinitionAuthoringDialog(
        services_.definition_catalogs, *config_session_, services_.config_repository, this);
    QObject::connect(definition_authoring_dialog_, &fastecu::ui::DefinitionAuthoringDialog::logE, log_channel_,
                     &fastecu::ui::LogChannel::logE);
    QObject::connect(definition_authoring_dialog_, &fastecu::ui::DefinitionAuthoringDialog::logW, log_channel_,
                     &fastecu::ui::LogChannel::logW);
    QObject::connect(definition_authoring_dialog_, &fastecu::ui::DefinitionAuthoringDialog::logI, log_channel_,
                     &fastecu::ui::LogChannel::logI);
    QObject::connect(definition_authoring_dialog_, &fastecu::ui::DefinitionAuthoringDialog::logD, log_channel_,
                     &fastecu::ui::LogChannel::logD);

    emit enableLogWriteToFile(true);

    QObject::connect(calibration_tree_widget_, &CalibrationTreeWidget::logE, log_channel_,
                     &fastecu::ui::LogChannel::logE);
    QObject::connect(calibration_tree_widget_, &CalibrationTreeWidget::logW, log_channel_,
                     &fastecu::ui::LogChannel::logW);
    QObject::connect(calibration_tree_widget_, &CalibrationTreeWidget::logI, log_channel_,
                     &fastecu::ui::LogChannel::logI);
    QObject::connect(calibration_tree_widget_, &CalibrationTreeWidget::logD, log_channel_,
                     &fastecu::ui::LogChannel::logD);

    // Before this window was built, DesktopComposition initialized the session
    // and the startup vehicle gate made sure a vehicle is selected.
    emit logD("Vehicle ID: " + qs(config_session_->Settings().selected_vehicle_id) + "/" +
                  QString::number(config_session_->Vehicles().size()),
              true, true);

    emit logD(qs(selectedVehicle().make), true, true);
    emit logD(protocolField(selectedVehicle(), &ProtocolSpec::mcu), true, true);
    emit logD(checksumField(selectedVehicle()), true, true);
    emit logD(qs(selectedVehicle().model), true, true);
    emit logD(qs(selectedVehicle().version), true, true);
    emit logD(qs(selectedVehicle().protocol->name), true, true);
    emit logD(protocolField(selectedVehicle(), &ProtocolSpec::description), true, true);
    emit logD(qs(config_session_->Settings().selected_flash_transport), true, true);
    emit logD(qs(config_session_->Settings().selected_log_transport), true, true);
    emit logD(qs(config_session_->Settings().selected_log_protocol), true, true);
    emit logD("ECU protocols set", true, true);

    QRect qrect = MainWindow::geometry();

    if (const fastecu::config::AppConfig& windowSettings = config_session_->Settings();
        windowSettings.window_width != "maximized" && windowSettings.window_height != "maximized")
    {
        this->setGeometry(qrect.x(), qrect.y(), qs(windowSettings.window_width).toInt(),
                          qs(windowSettings.window_height).toInt());
    }
    else
    {
        this->setWindowState(Qt::WindowMaximized);
    }

    if (config_session_->Settings().romraider_definition_files.empty() &&
        config_session_->Settings().ecuflash_definition_files_directory.empty())
    {
        QMessageBox::warning(this, tr("Ecu definition file"),
                             "No definition file(s), use 'Settings' in 'Edit' menu to choose file(s)");
    }

    // Scan errors are nonfatal and already reported by the session.
    std::ignore = services_.definition_catalogs.RefreshIndex(fastecu::definition::DefinitionFormat::kEcuFlash);

    // Scan errors are nonfatal and already reported by the session.
    std::ignore = services_.definition_catalogs.RefreshIndex(fastecu::definition::DefinitionFormat::kRomRaider);

    if (const QString kernelDir = qs(config_session_->EffectivePaths().kernel_files_directory);
        QDir(kernelDir).exists())
    {
        QDir dir(kernelDir);
        QStringList nameFilter("*.bin");
        QStringList txtFilesAndDirectories = dir.entryList(nameFilter);
        // emit LOG_D(txtFilesAndDirectories;
    }

    applyStandardShortcuts();
    connectMenuActions();

    connect(ui_->switchBoxWidget, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(changeSwitchValues()));
    connect(ui_->logBoxWidget, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(changeDigitalValues()));
    connect(ui_->mdiArea, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(changeGaugeValues()));
    // connect(ui->action_Preferences, SIGNAL(triggered()), this, SLOT(openPreferences()));
    connect(ui_->calibrationFilesTreeWidget, SIGNAL(itemClicked(QTreeWidgetItem *, int)), this,
            SLOT(calibrationFilesTreewidgetItemSelected(QTreeWidgetItem *)));
    connect(ui_->calibrationDataTreeWidget, SIGNAL(itemClicked(QTreeWidgetItem *, int)), this,
            SLOT(calibrationDataTreewidgetItemSelected(QTreeWidgetItem *)));
    connect(ui_->calibrationDataTreeWidget, SIGNAL(itemExpanded(QTreeWidgetItem *)), this,
            SLOT(calibrationDataTreewidgetItemExpanded(QTreeWidgetItem *)));
    connect(ui_->calibrationDataTreeWidget, SIGNAL(itemCollapsed(QTreeWidgetItem *)), this,
            SLOT(calibrationDataTreewidgetItemCollapsed(QTreeWidgetItem *)));
    connect(calibration_tree_widget_, SIGNAL(closeRom()), this, SLOT(closeCalibration()));

    status_bar_connection_label_->setMargin(5);
    // status_bar_connection_label->setStyleSheet("QLabel { background-color : red; color : white; }");
    setStatusBarLabel(false, false, "");

    status_bar_ecu_label_->setText("");
    status_bar_ecu_label_->setMargin(5);
    status_bar_ecu_label_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    // status_bar_ecu_label->setStyleSheet("QLabel { background-color : red; color : white; }");

    QWidget *statusBarSpacer = new QWidget();
    statusBarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    statusBar()->addWidget(status_bar_connection_label_);
    statusBar()->addWidget(statusBarSpacer);
    statusBar()->addPermanentWidget(status_bar_ecu_label_);
    statusBar()->setSizeGripEnabled(true);

    ui_->calibrationFilesTreeWidget->setHeaderLabel("Calibration Files");
    ui_->calibrationDataTreeWidget->setHeaderLabel("Calibration Data");
    ui_->calibrationDataTreeWidget->resizeColumnToContents(0);
    ui_->calibrationDataTreeWidget->resizeColumnToContents(1);
    ui_->calibrationFilesTreeWidget->setMinimumHeight(125);
    ui_->calibrationFilesTreeWidget->minimumHeight();
    ui_->calibrationFilesTreeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui_->calibrationFilesTreeWidget, SIGNAL(customContextMenuRequested(QPoint)),
            SLOT(customMenuRequested(QPoint)));

    // ui->splitter->setStretchFactor(2, 1);
    ui_->splitter->setSizes(QList<int>({125, INT_MAX}));

    // Splash screen
    net_splash_ = new QSplashScreen();
    QVBoxLayout *netSplashLayout = new QVBoxLayout(net_splash_);
    netSplashLayout->setAlignment(Qt::AlignCenter);
    QLabel *netSplashLabel = new QLabel(QString("Waiting for peer " + peerAddress + "..."), net_splash_);
    netSplashLabel->setAlignment(Qt::AlignCenter);
    QProgressBar *netSplashProgressBar = new QProgressBar(net_splash_);
    netSplashProgressBar->setAlignment(Qt::AlignCenter);
    netSplashProgressBar->setMinimum(0);
    // Number of network connection stages
    netSplashProgressBar->setMaximum(2);
    netSplashProgressBar->setValue(0);
    QPushButton *btnCloseApp = new QPushButton("Close app", net_splash_);
    netSplashLayout->addWidget(netSplashLabel);
    netSplashLayout->addWidget(netSplashProgressBar);
    netSplashLayout->addWidget(btnCloseApp);
    // splash->setLayout(layout);
    net_splash_->resize(350, 50);
    // Show it in remote mode only
    // Prepare for remote mode
    if (!peerAddress.isEmpty())
    {
        net_splash_->show();
        // Move splashscreen to the center of the screen
        QScreen *screenLocal = QGuiApplication::primaryScreen();
        QRect screenGeometryLocal = screenLocal->geometry();
        net_splash_->move(screenGeometryLocal.center() - net_splash_->rect().center());
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

    connection_ = &services_.connection;
    remote_peer_ = &services_.remote;
    if (!peerAddress.isEmpty())
    {
        netSplashProgressBar->setValue(0);
        netSplashProgressBar->setFormat("Connecting to J2534 and serial devices...");
        connection_->WaitForSource();
        netSplashProgressBar->setValue(1);
        netSplashProgressBar->setFormat("Connecting to utility functions...");
        remote_peer_->waitForSource();
        netSplashProgressBar->setValue(2);
    }
    externalLogger("Connection successfull.");

    timer->stop();
    net_splash_->close();
    timer->deleteLater();
    connect(connection_, &fastecu::desktop::connection::AdapterConnection::stateChanged, this,
            &MainWindow::networkStateChanged, Qt::DirectConnection);
    connect(remote_peer_, &fastecu::ui::RemotePeer::stateChanged, this, &MainWindow::networkStateChanged,
            Qt::DirectConnection);

    // Set timer to read vbatt value
    vbatt_timer_ = new QTimer(this);
    vbatt_timer_->setInterval(vbatt_timer_timeout_);
    connect(vbatt_timer_, SIGNAL(timeout()), this, SLOT(updateVbatt()));

    toolbar_item_size_.setWidth(qs(config_session_->Settings().toolbar_iconsize).toInt());
    toolbar_item_size_.setHeight(qs(config_session_->Settings().toolbar_iconsize).toInt());
    ui_->toolBar->setIconSize(toolbar_item_size_);

    QWidget *spacer = new QWidget();
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui_->toolBar->addWidget(spacer);

    ui_->toolBar->addSeparator();

    QPushButton *selectProtocolButton = new QPushButton();
    selectProtocolButton->setText("Select protocol");
    // car_make_button->setMargin(10);
    selectProtocolButton->setFixedHeight(toolbar_item_size_.height());
    ui_->toolBar->addWidget(selectProtocolButton);
    connect(selectProtocolButton, SIGNAL(clicked(bool)), this, SLOT(selectProtocol()));

    QPushButton *selectVehicleButton = new QPushButton();
    selectVehicleButton->setText("Select vehicle");
    // car_make_button->setMargin(10);
    selectVehicleButton->setFixedHeight(toolbar_item_size_.height());
    ui_->toolBar->addWidget(selectVehicleButton);
    connect(selectVehicleButton, SIGNAL(clicked(bool)), this, SLOT(selectVehicle()));

    flash_transport_list_ = new QComboBox();
    flash_transport_list_->setFixedHeight(toolbar_item_size_.height());
    flash_transport_list_->setFixedWidth(90);
    flash_transport_list_->setObjectName("flash_transport_list");
    ui_->toolBar->addWidget(flash_transport_list_);

    ui_->toolBar->addSeparator();

    QLabel *logTransport = new QLabel("Log:");
    logTransport->setMargin(10);
    ui_->toolBar->addWidget(logTransport);

    log_transport_list_ = new QComboBox();
    log_transport_list_->setFixedHeight(toolbar_item_size_.height());
    log_transport_list_->setFixedWidth(90);
    log_transport_list_->setObjectName("log_transport_list");
    ui_->toolBar->addWidget(log_transport_list_);

    flash_transports_ = createFlashTransportsList();
    log_transports_ = createLogTransportsList();
    connect(flash_transport_list_, SIGNAL(currentIndexChanged(int)), this, SLOT(flashTransportChanged()));
    connect(log_transport_list_, SIGNAL(currentIndexChanged(int)), this, SLOT(logTransportChanged()));

    // ui->toolBar->addSeparator();

    QLabel *logSelect = new QLabel();
    logSelect->setMargin(5);
    ui_->toolBar->addWidget(logSelect);

    ecu_radio_button_ = new QRadioButton("ECU");
    ecu_radio_button_->setChecked(true);
    ui_->toolBar->addWidget(ecu_radio_button_);
    tcu_radio_button_ = new QRadioButton("TCU");
    ui_->toolBar->addWidget(tcu_radio_button_);

    ui_->toolBar->addSeparator();

    QLabel *serialPortSelect = new QLabel("Port:");
    serialPortSelect->setMargin(10);
    ui_->toolBar->addWidget(serialPortSelect);

    serial_port_list_ = new QComboBox();
    serial_port_list_->setFixedHeight(toolbar_item_size_.height());
    serial_port_list_->setFixedWidth(180);
    serial_port_list_->setObjectName("serial_port_list");
    serial_ports_ = connection_->AvailablePorts();
    for (int i = 0; i < serial_ports_.length(); i++)
    {
        serial_port_list_->addItem(serial_ports_.at(i));
        if (qs(config_session_->Settings().serial_port) == serial_ports_.at(i).split(" - ").at(0))
        {
            serial_port_list_->setCurrentIndex(i);
        }
    }
    ui_->toolBar->addWidget(serial_port_list_);

    refresh_serial_port_list_ = new QPushButton();
    refresh_serial_port_list_->setIcon(QIcon(":/icons/view-refresh.png"));
    refresh_serial_port_list_->setFixedHeight(toolbar_item_size_.height());
    refresh_serial_port_list_->setFixedWidth(toolbar_item_size_.height());
    // refresh_serial_port_list->setIconSize(toolbar_item_size);
    connect(refresh_serial_port_list_, SIGNAL(clicked(bool)), this, SLOT(checkSerialPorts()));
    ui_->toolBar->addWidget(refresh_serial_port_list_);

    logger_model_ = &services_.logger_model;
    loadLoggerDefinition();
    logger_values_.Initialize(*logger_model_);
    log_boxes_ = new LogBox();

    if (logger_model_ != nullptr)
    {
        updateLogboxes(qs(config_session_->Settings().selected_log_protocol));
    }

    serial_port_ = serial_port_prefix_ + qs(config_session_->Settings().serial_port);
    serial_port_baudrate_ = default_serial_port_baudrate_;
    connection_->SetInitialPort(serial_port_, serial_port_baudrate_);
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
    log_file_timer_ = std::make_unique<QElapsedTimer>();

    if (!calibrations_.empty())
    {
        const QModelIndex index = ui_->calibrationFilesTreeWidget->selectionModel()->currentIndex();
        emit ui_->calibrationFilesTreeWidget->clicked(index);
    }

    emit log_transport_list_->currentIndexChanged(log_transport_list_->currentIndex());

    status_bar_ecu_label_->setText(protocolField(selectedVehicle(), &ProtocolSpec::description) + " ");

    setFlashArrowState();

    net_splash_->deleteLater();
    emit logI("FastECU initialized", true, true);
}

void MainWindow::emitLogLine(fastecu::LogLevel level, const QString& message)
{
    switch (level)
    {
    case fastecu::LogLevel::kError:
        emit logE(message, true, true);
        break;
    case fastecu::LogLevel::kWarning:
        emit logW(message, true, true);
        break;
    case fastecu::LogLevel::kInfo:
        emit logI(message, true, true);
        break;
    case fastecu::LogLevel::kDebug:
        emit logD(message, true, true);
        break;
    }
}

MainWindow::~MainWindow()
{
    connection_coordinator_->shutdown();
    if (logging_state_)
    {
        logging_engine_->Stop();
    }
}

void MainWindow::networkStateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState)
{
    if (state == QRemoteObjectReplica::Valid)
    {
        emit logD("Network connection established", true, true);
    }
    else if (oldState == QRemoteObjectReplica::Valid)
    {
        if (!restart_question_active_.tryLock())
        {
            return;
        }
        emit logD("Network connection lost, reconnecting...", true, true);
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

        restart_question_active_.unlock();
    }
}

void MainWindow::setComboBoxItemEnabled(QComboBox *comboBox, int index, bool enabled)
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

QStringList MainWindow::createFlashTransportsList()
{
    QStringList flashProtocols;

    flashProtocols.append(protocolField(selectedVehicle(), &ProtocolSpec::flash_transport).split(","));

    flash_transport_list_->clear();
    for (int i = 0; i < flashProtocols.length(); i++)
    {
        flash_transport_list_->addItem(flashProtocols.at(i));
        if (qs(config_session_->Settings().selected_flash_transport) == flashProtocols.at(i))
        {
            flash_transport_list_->setCurrentIndex(i);
        }
    }
    return flashProtocols;
}

QStringList MainWindow::createLogTransportsList()
{
    QStringList logTransportsLocal;

    logTransportsLocal.append(protocolField(selectedVehicle(), &ProtocolSpec::log_transport).split(","));

    log_transport_list_->clear();
    for (int i = 0; i < logTransportsLocal.length(); i++)
    {
        log_transport_list_->addItem(logTransportsLocal.at(i));
        if (qs(config_session_->Settings().selected_flash_transport) == logTransportsLocal.at(i))
        {
            log_transport_list_->setCurrentIndex(i);
            protocol_ = "SSM";
        }
    }

    // if (car_model_list->currentText() == "Subaru")
    // protocol = "SSM";

    return logTransportsLocal;
}

const fastecu::config::VehicleSpec& MainWindow::selectedVehicle() const
{
    // The startup vehicle gate selects a vehicle before MainWindow is built,
    // and a selection only ever changes to another valid row.
    return *config_session_->SelectedVehicle();
}

void MainWindow::saveSettings()
{
    if (const fastecu::Status saved = config_session_->Save(); !saved.has_value())
    {
        if (last_settings_save_error_ != saved.error())
        {
            last_settings_save_error_ = saved.error();
            emit logE(qs(saved.error().detail), true, true);
        }
    }
    else
    {
        last_settings_save_error_.reset();
    }
}

void MainWindow::applyVehicleChoice(int result, std::optional<std::size_t> row)
{
    if (result == QDialog::Accepted && row.has_value())
    {
        if (const fastecu::Status selected = config_session_->SelectRow(*row); !selected.has_value())
        {
            emit logE(qs(selected.error().detail), true, true);
        }
    }
    selectVehicleFinished(result);
}

void MainWindow::applyProtocolChoice(int result, std::optional<std::string> protocolName)
{
    if (result == QDialog::Accepted && protocolName.has_value())
    {
        config_session_->SelectByProtocolName(*protocolName);
    }
    selectProtocolFinished(result);
}

void MainWindow::selectProtocol()
{
    ProtocolSelect protocolSelect(*config_session_);
    const int result = protocolSelect.exec();
    applyProtocolChoice(result, protocolSelect.acceptedProtocolName());
    emit logD("Selected vehicle: " + qs(config_session_->Settings().selected_vehicle_id), true, true);
}

void MainWindow::selectProtocolFinished(int result)
{
    if (result == QDialog::Accepted)
    {
        createFlashTransportsList();
        createLogTransportsList();
        saveSettings();

        setFlashArrowState();
    }
    else
    {
        // emit LOG_D("Dialog is rejected";
    }

    status_bar_ecu_label_->setText(protocolField(selectedVehicle(), &ProtocolSpec::description) + " ");
}

void MainWindow::selectVehicle()
{
    VehicleSelect vehicleSelect(*config_session_);
    const int result = vehicleSelect.exec();
    applyVehicleChoice(result, vehicleSelect.chosenRow());
    emit logD("Selected vehicle: " + qs(config_session_->Settings().selected_vehicle_id), true, true);
}

void MainWindow::selectVehicleFinished(int result)
{
    if (result == QDialog::Accepted)
    {
        createFlashTransportsList();
        createLogTransportsList();
        saveSettings();

        setFlashArrowState();
    }
    else
    {
        // emit LOG_D("Dialog is rejected";
    }

    status_bar_ecu_label_->setText(protocolField(selectedVehicle(), &ProtocolSpec::description) + " ");
}

MainWindow::OpenCalibration *MainWindow::openCalibration(fastecu::calibration::SessionId id)
{
    const auto found = std::ranges::find_if(calibrations_, [id](const OpenCalibration& open) { return open.id == id; });
    return found == calibrations_.end() ? nullptr : &*found;
}

MainWindow::OpenCalibration *MainWindow::selectedOpenCalibration()
{
    const QList<QTreeWidgetItem *> selected = ui_->calibrationFilesTreeWidget->selectedItems();
    const auto id = selected.isEmpty() ? std::nullopt : sessionOf(selected.at(0));
    return id.has_value() ? openCalibration(*id) : nullptr;
}

fastecu::calibration::CalibrationSession *MainWindow::calibration(fastecu::calibration::SessionId id)
{
    OpenCalibration *open = openCalibration(id);
    return open != nullptr ? calibration_workspace_->Find(open->id) : nullptr;
}

std::optional<fastecu::calibration::SessionId> MainWindow::sessionOf(const QTreeWidgetItem *filesItem) const
{
    return filesItem == nullptr ? std::nullopt : fastecu::ui::parseSessionKey(filesItem->text(2));
}

fastecu::calibration::CalibrationSession *MainWindow::selectedCalibration()
{
    OpenCalibration *open = selectedOpenCalibration();
    return open != nullptr ? calibration_workspace_->Find(open->id) : nullptr;
}

QTreeWidgetItem *MainWindow::filesTreeItem(fastecu::calibration::SessionId id) const
{
    for (int i = 0; i < ui_->calibrationFilesTreeWidget->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem *item = ui_->calibrationFilesTreeWidget->topLevelItem(i);
        if (sessionOf(item) == id)
        {
            return item;
        }
    }
    return nullptr;
}

// Presents the newly opened session with the protocol refresh, missing-
// definition prompt, and both trees in the established order.
bool MainWindow::addCalibration(fastecu::calibration::SessionId id)
{
    const fastecu::calibration::CalibrationSession *session = calibration_workspace_->Find(id);
    if (session == nullptr)
    {
        return false;
    }
    calibrations_.push_back(OpenCalibration{.id = id});

    updateProtocolInfo(
        fastecu::ui::romInfoValue(fastecu::ui::romInfoValues(*session), fastecu::ui::RomInfoRow::kFlashMethod));
    if (session->Definition() == nullptr)
    {
        promptForMissingDefinition(id);
    }
    const OpenCalibration *open = openCalibration(id);
    calibration_tree_widget_->buildCalibrationFilesTree(id, ui_->calibrationFilesTreeWidget, *session);
    calibration_tree_widget_->buildCalibrationDataTree(ui_->calibrationDataTreeWidget, *session, open->view);
    return true;
}

void MainWindow::updateProtocolInfo(const QString& flashMethod)
{
    // Mirrored in CalibrationOperationCoordinator::refresh_write_metadata; keep the two in sync.
    emit logD("Update protocol info by selected ROM with FlashMethod: " + flashMethod, true, true);
    // The last matching row wins, as the legacy scan did; no match changes
    // nothing.
    if (const bool infoUpdated = config_session_->SelectByProtocolName(flashMethod.toStdString()); infoUpdated)
    {
        emit logD("Protocol info for selected ROM updated", true, true);
    }
    else
    {
        emit logD("Could not find protocol for selected ROM!", true, true);
    }
    status_bar_ecu_label_->setText(protocolField(selectedVehicle(), &ProtocolSpec::description) + " ");
}

void MainWindow::setFlashArrowState()
{
    ui_->actionReadRomFromEcu->setEnabled(protocolCapability(selectedVehicle(), &ProtocolSpec::read));
    ui_->actionTestWriteRomToEcu->setEnabled(protocolCapability(selectedVehicle(), &ProtocolSpec::test_write));
    ui_->actionWriteRomToEcu->setEnabled(protocolCapability(selectedVehicle(), &ProtocolSpec::write));
}

void MainWindow::logTransportChanged()
{
    connection_coordinator_->cancel();
    // emit LOG_D("Change log transport";
    QComboBox *logTransportList = ui_->toolBar->findChild<QComboBox *>("log_transport_list");

    connection_->ApplyLogTransport(fastecu::desktop::connection::LogTransportFromText(logTransportList->currentText()),
                                   config_session_->Settings().selected_log_protocol == "SSM");

    protocol_ = qs(config_session_->Settings().selected_log_protocol);
    config_session_->Settings().selected_log_transport = logTransportList->currentText().toStdString();
    saveSettings();

    ecuid_.clear();
    ecu_init_complete_ = false;
    // ssm_init_poll_timer->start();
}

void MainWindow::flashTransportChanged()
{
    // emit LOG_D("Change flash transport";
    QComboBox *flashTransportList = ui_->toolBar->findChild<QComboBox *>("flash_transport_list");

    config_session_->Settings().selected_flash_transport = flashTransportList->currentText().toStdString();
    saveSettings();
}

void MainWindow::checkSerialPorts()
{
    connection_coordinator_->cancel();
    QComboBox *serialPortList = ui_->toolBar->findChild<QComboBox *>("serial_port_list");
    QString prevSerialPort = serialPortList->currentText();
    int index = 0;

    // serial_poll_timer->stop();
    // ssm_init_poll_timer->stop();

    connection_->ClearLinkFlags();
    ecuid_.clear();
    ecu_init_complete_ = false;
    emit log_transport_list_->currentIndexChanged(log_transport_list_->currentIndex());

    // QStringList j2534_list = serial->getAvailableJ2534Libs();
    // emit LOG_D("J2534 Vehicle PassThru Interfaces:" << j2534_list;

    serial_ports_ = connection_->AvailablePorts();
    serialPortList->clear();

    for (int i = 0; i < serial_ports_.length(); i++)
    {
        serialPortList->addItem(serial_ports_.at(i));
        if (prevSerialPort == serial_ports_.at(i))
        {
            serialPortList->setCurrentIndex(index);
        }
        index++;
    }

    // emit LOG_D("Start serial and ssm poll timers";
    // serial_poll_timer->start();
    // ssm_init_poll_timer->start();
}

void MainWindow::openSerialPort()
{
    connection_coordinator_->cancel();
    const QString port = selectedSerialPort();
    if (port.isEmpty())
    {
        return;
    }
    connection_->SelectPort(port);
    QString openedSerialPort = connection_->Open();
    if (openedSerialPort != "")
    {
        rememberOpenedPort(port, openedSerialPort);
        if (ecuid_ == "")
        {
            setStatusBarLabel(true, false, "");
        }
        else
        {
            setStatusBarLabel(true, true, ecuid_);
        }
    }
    else
    {
        setStatusBarLabel(false, false, "");
        ecu_init_complete_ = false;
    }
}

void MainWindow::rememberOpenedPort(const QString& port, const QString& openedPort)
{
    if (openedPort != previous_serial_port_)
    {
        ecuid_.clear();
        ecu_init_complete_ = false;
    }
    previous_serial_port_ = openedPort;
    config_session_->Settings().serial_port = port.toStdString();
    saveSettings();
}

int MainWindow::startEcuOperations(const QString& cmdType)
{
    connection_coordinator_->cancel();
    setRealtimeState(false);
    toggleRealtime();

    std::optional<fastecu::ui::PreparedWrite> preparedWrite;
    QString readKernelPath;
    QString readKernelAddress;

    QComboBox *serialPortList = ui_->toolBar->findChild<QComboBox *>("serial_port_list");
    if (serialPortList->currentText() == "")
    {
        emit logD("No serial port selected!", true, true);
        QMessageBox::warning(this, tr("Serial port"), "No serial port selected!");
        return 0;
    }

    connection_->SelectPort(selectedSerialPort());

    // A local copy: the provisioned kernel directory is never rewritten.
    QString kernelDir = qs(config_session_->EffectivePaths().kernel_files_directory);
    if (!kernelDir.endsWith('/'))
    {
        kernelDir.append("/");
    }

    // Every path from here on -- including the write-preflight early returns
    // and the Denso TCU service-action return -- restores the serial facade
    // and stops battery polling on exit.
    const auto cleanup = qScopeGuard(
        [this]
        {
            vbatt_timer_->stop();
            fastecu::desktop::serial::ResetSerialToIdle(connection_->Facade());
            ecuid_.clear();
            ecu_init_complete_ = false;
            emit log_transport_list_->currentIndexChanged(log_transport_list_->currentIndex());
            connection_->SetPortSpeed(4800);
        });

    if (selectedVehicle().make == "Subaru" || selectedVehicle().make == "Mitsubishi")
    {
        fastecu::desktop::serial::ResetSerialToIdle(connection_->Facade());
        ecuid_.clear();
        ecu_init_complete_ = false;

        updateVbatt();
        vbatt_timer_->start();

        if (!kernelDir.endsWith('/'))
        {
            kernelDir.append("/");
        }

        if (cmdType == "test_write" || cmdType == "write")
        {
            preparedWrite = calibration_operations_->prepareWrite(selectedCalibration(), kernelDir.toStdString());
            if (!preparedWrite.has_value())
            {
                return 0;
            }
        }
        else
        {
            // Nothing is allocated before the read: the image is adopted into
            // a session only after it succeeded.
            updateProtocolInfo(qs(selectedVehicle().protocol->name));
            readKernelPath = QString::fromStdString(
                fastecu::flash::KernelPath(kernelDir.toStdString(), selectedVehicle().protocol->kernel));
            readKernelAddress = kernelAddressField(selectedVehicle());
        }

        emit logD("Protocol to use: " + qs(selectedVehicle().protocol->name), true, true);

        const fastecu::flash::FlashOperation operation =
            fastecu::flash::FlashOperationFromCommand(cmdType.toStdString());

        fastecu::flash::FlashOperationController controller{connection_->Facade(), this};
        // Relay through MainWindow's own LOG_* signals, like every UI logger;
        // see LogChannel for why lines go through a long-lived sender.
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::logE, this, &MainWindow::logE);
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::logW, this, &MainWindow::logW);
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::logI, this, &MainWindow::logI);
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::logD, this, &MainWindow::logD);
        QObject::connect(&controller, qOverload<QString>(&fastecu::flash::FlashOperationController::externalLogger),
                         this, &MainWindow::externalLogger);
        QObject::connect(&controller, qOverload<int>(&fastecu::flash::FlashOperationController::externalLogger), this,
                         &MainWindow::externalLoggerSetProgressbarValue);

        const fastecu::flash::FlashOperationOutcome outcome = controller.run({
            .operation = operation,
            .protocol = preparedWrite.has_value() ? preparedWrite->protocol : *selectedVehicle().protocol,
            .kernel_path = preparedWrite.has_value() ? preparedWrite->kernel_path : readKernelPath.toStdString(),
            .image = fastecu::flash::PortableImageForOperation(
                operation, preparedWrite.has_value() ? bytes::ByteView{preparedWrite->image} : bytes::ByteView{}),
            .paths = config_session_->EffectivePaths(),
            .display_filename = preparedWrite.has_value() ? preparedWrite->display_filename : std::string{},
        });

        if (outcome.status == fastecu::flash::FlashOperationStatus::kServiceActionHandled)
        {
            // The old goto skipped the post-operation block entirely.
        }
        else if (cmdType == "read")
        {
            if (outcome.status == fastecu::flash::FlashOperationStatus::kCompleted && outcome.read_bytes &&
                !outcome.read_bytes->empty())
            {
                const QString dateTimeString = QDateTime::currentDateTime().toString("yyyy-MM-dd_hh'h'mm'm'ss's'");
                const std::string romId = outcome.rom_id.value_or(std::string{});
                const auto opened = calibration_workspace_->AdoptReadImage(fastecu::calibration::ReadImage{
                    .rom = *outcome.read_bytes,
                    .filename = fastecu::flash::ReadImageFilename(romId, dateTimeString.toStdString()),
                    .rom_id = romId,
                    .protocol_name = std::string(selectedVehicle().protocol->name),
                    .kernel_path = readKernelPath.toStdString(),
                    .kernel_start_address = readKernelAddress.toStdString(),
                });
                if (opened.has_value() && addCalibration(opened->id))
                {
                    saveCalibrationFileAs();
                }
            }
        }
    }
    return 0;
}

void MainWindow::customMenuRequested(QPoint pos)
{
    if (calibrations_.empty())
    {
        emit logD("No calibration to select!", true, true);
        return;
    }
    QModelIndex index = ui_->calibrationFilesTreeWidget->indexAt(pos);
    emit ui_->calibrationFilesTreeWidget->clicked(index);

    QMenu *menu = new QMenu(this);
    // menu->addAction("Sync with ECU", this, SLOT(syncCalWithEcu()));
    menu->addAction("Close selected ROM", this, SLOT(closeCalibration()));
    menu->popup(ui_->calibrationFilesTreeWidget->viewport()->mapToGlobal(pos));
}

bool MainWindow::openCalibrationFile(QString filename)
{
    if (filename.isEmpty())
    {
        QFileDialog openDialog;
        openDialog.setDefaultSuffix("bin");
        filename = QFileDialog::getOpenFileName(this, tr("Open ROM file"),
                                                qs(config_session_->EffectivePaths().calibration_files_directory),
                                                tr("Calibration file (*.bin *.hex)"));
        if (filename.isEmpty())
        {
            QMessageBox::information(this, tr("Calibration file"), "No file selected");
            return 1; // matches open_subaru_rom_file's old "no file selected" return of nullptr -> caller's failure
                      // path
        }
    }

    const auto opened = calibration_workspace_->OpenFile(filename.toStdString());
    if (!opened.has_value())
    {
        return 1;
    }
    return addCalibration(opened->id) ? 0 : 1;
}

void MainWindow::promptForMissingDefinition(fastecu::calibration::SessionId id)
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
            emit logD(createNewRadioButton->text(), true, true);
            definition_authoring_dialog_->createNewDefinition();
        }
        else if (useExistingRadioButton->isChecked())
        {
            emit logD(useExistingRadioButton->text(), true, true);
            definition_authoring_dialog_->useExistingDefinition();
        }
    }
    // The "continue without definition" placeholders belong to this branch
    // only -- a user who picked create-new or use-existing must not get them
    // stamped over the definition they just provided.
    if (continueWithoutRadioButton->isChecked() || result == QDialog::Rejected)
    {
        emit logD(continueWithoutRadioButton->text(), true, true);
        if (OpenCalibration *open = openCalibration(id); open != nullptr)
        {
            open->view.missing_definition_make = qs(selectedVehicle().make);
        }
    }
}

void MainWindow::saveCalibrationFile()
{
    // The coordinator reports every outcome; saving in place changes no label.
    calibration_operations_->save(selectedCalibration(), fastecu::ui::SaveMode::kSave);
}

void MainWindow::saveCalibrationFileAs()
{
    // Resolved once: the selection can change while the picker is open, and
    // the label belongs to the session that was saved.
    fastecu::calibration::CalibrationSession *session = selectedCalibration();
    if (calibration_operations_->save(session, fastecu::ui::SaveMode::kSaveAs) != fastecu::ui::SaveOutcome::kSaved)
    {
        return;
    }
    if (QTreeWidgetItem *item = filesTreeItem(session->Id()); item != nullptr)
    {
        item->setText(0, qs(session->Source().display_name));
    }
}

void MainWindow::selectableComboboxItemChanged(const QString& item)
{
    const auto id = fastecu::ui::parseMapWindowId(ui_->mdiArea->activeSubWindow());
    if (id.has_value())
    {
        setMapSelection(id->session, id->map_number, item);
    }
}

void MainWindow::setMapSelection(fastecu::calibration::SessionId id, int mapIndex, const QString& item)
{
    if (mapIndex < 0)
    {
        return;
    }
    const auto outcome = fastecu::calibration::ApplySelectableEdit(
        *calibration_workspace_,
        {.session = id, .map_index = static_cast<std::size_t>(mapIndex), .selection = item.toStdString()});
    if (!outcome.has_value())
    {
        QMessageBox::warning(this, tr("Set value"), qs(outcome.error().detail));
        return;
    }
    // A name no selection carries still refreshes, so the combo snaps back to the ROM's bytes.
    if (const auto *skipped = std::get_if<fastecu::calibration::SelectableEditNotApplicable>(&*outcome);
        skipped != nullptr && skipped->reason != fastecu::calibration::SelectableNotApplicableReason::kUnknownSelection)
    {
        return;
    }
    for (auto *window : ui_->mdiArea->subWindowList())
    {
        const auto identity = fastecu::ui::parseMapWindowId(window);
        if (identity.has_value() && identity->session == id && identity->map_number == mapIndex)
        {
            if (auto *mapWindow = qobject_cast<CalibrationMaps *>(window->widget()); mapWindow != nullptr)
            {
                mapWindow->refresh();
            }
        }
    }
}

void MainWindow::checkboxStateChanged(int state)
{
    const auto id = fastecu::ui::parseMapWindowId(ui_->mdiArea->activeSubWindow());
    if (id.has_value())
    {
        setMapSwitch(id->session, id->map_number, state);
    }
}

void MainWindow::setMapSwitch(fastecu::calibration::SessionId id, int mapIndex, int state)
{
    // No retained definition format populated StateList. RomRaider switches
    // resolve to bloblist Selectable and use set_map_selection. Preserve the
    // empty legacy switch path without adding definition support.
    if (calibration_workspace_->Find(id) == nullptr)
    {
        return;
    }
    (void)mapIndex;
    (void)state;
}

void MainWindow::calibrationFilesTreewidgetItemSelected(QTreeWidgetItem *item)
{
    QList<QTreeWidgetItem *> itemList;
    QTreeWidgetItem *selectedItem = ui_->calibrationFilesTreeWidget->selectedItems().at(0);

    for (int i = 0; i < ui_->calibrationFilesTreeWidget->topLevelItemCount(); i++)
    {
        QTreeWidgetItem *itemLocal = ui_->calibrationFilesTreeWidget->topLevelItem(i);
        itemLocal->setCheckState(0, Qt::Unchecked);
    }

    selectedItem->setCheckState(0, Qt::Checked);

    QString romId = selectedItem->text(1);

    OpenCalibration *open = selectedOpenCalibration();
    const fastecu::calibration::CalibrationSession *session =
        open != nullptr ? calibration_workspace_->Find(open->id) : nullptr;
    if (session == nullptr)
    {
        return;
    }
    calibration_tree_widget_->buildCalibrationDataTree(ui_->calibrationDataTreeWidget, *session, open->view);
    updateProtocolInfo(
        fastecu::ui::romInfoValue(fastecu::ui::romInfoValues(*session), fastecu::ui::RomInfoRow::kFlashMethod));
}

void MainWindow::calibrationDataTreewidgetItemSelected(QTreeWidgetItem *item)
{
    const QModelIndex index = ui_->calibrationDataTreeWidget->selectionModel()->currentIndex();
    QString selectedText = index.data(Qt::DisplayRole).toString();
    QString selectedRom;

    selectedText = item->text(0);

    // A top-level item is a category header; selecting it opens nothing.
    if (ui_->calibrationDataTreeWidget->indexOfTopLevelItem(item) > -1)
    {
        return;
    }
    if (ui_->calibrationDataTreeWidget->indexOfTopLevelItem(item->parent()) > -1)
    {
        QTreeWidgetItem *selectedFilesTreeItem = ui_->calibrationFilesTreeWidget->selectedItems().at(0);
        QTreeWidgetItem *selectedDataTreeItem = item;
        const auto session = sessionOf(selectedFilesTreeItem);
        OpenCalibration *open = session.has_value() ? openCalibration(*session) : nullptr;
        if (!session.has_value() || open == nullptr)
        {
            return;
        }
        const auto *rom = calibration_workspace_->Find(*session);
        if (rom == nullptr || rom->Definition() == nullptr)
        {
            return;
        }
        const auto& maps = rom->Definition()->definition.maps;
        int mapIndex = selectedDataTreeItem->text(1).toInt();
        for (int i = 0; i < static_cast<int>(maps.size()); i++)
        {
            const auto& name = maps[static_cast<std::size_t>(i)].name;
            const QString mapName = name.empty() ? QString(" ") : qs(name);
            if (mapName == selectedText && i == mapIndex)
            {
                if (open->view.open_maps.contains(static_cast<std::size_t>(i)))
                {
                    QList<QMdiSubWindow *> list = ui_->mdiArea->findChildren<QMdiSubWindow *>();
                    foreach (QMdiSubWindow *w, list)
                    {
                        if (w->objectName().startsWith(fastecu::ui::sessionKeyText(*session) + "," +
                                                       QString::number(i) + "," +
                                                       qs(maps[static_cast<std::size_t>(i)].name)))
                        {
                            if (w->objectName() == ui_->mdiArea->activeSubWindow()->objectName())
                            {
                                ui_->mdiArea->removeSubWindow(w);
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
                    const auto shown = fastecu::ui::presentMap(*rom, static_cast<std::size_t>(i));
                    if (!shown.has_value())
                    {
                        emit logE("Error decoding calibration map values [" +
                                      QString(fastecu::ToString(shown.error().kind)) + "]: " + qs(shown.error().detail),
                                  true, true);
                    }
                    open->view.open_maps.insert(static_cast<std::size_t>(i));
                    item->setCheckState(0, Qt::Checked);

                    CalibrationMaps *calibrationMaps =
                        new CalibrationMaps(*calibration_workspace_, *session, i, ui_->mdiArea->contentsRect());
                    QMdiSubWindow *subWindow = ui_->mdiArea->addSubWindow(calibrationMaps);
                    if (subWindow)
                    {
                        subWindow->setAttribute(Qt::WA_DeleteOnClose, true);
                        subWindow->setObjectName(calibrationMaps->objectName());
                        subWindow->show();
                        subWindow->adjustSize();
                        subWindow->move(0, 0);
                        // subWindow->setFixedWidth(subWindow->width());
                        // subWindow->setFixedHeight(subWindow->height());

                        connect(calibrationMaps, &CalibrationMaps::selectableComboboxItemChanged, this,
                                [this, id = *session, i](const QString& value) { setMapSelection(id, i, value); });
                        connect(calibrationMaps, &CalibrationMaps::checkboxStateChanged, this,
                                [this, id = *session, i](int state) { setMapSwitch(id, i, state); });
                        connect(subWindow, SIGNAL(destroyed(QObject *)), this, SLOT(closeCalibrationMap(QObject *)));
                    }
                }
            }
        }
    }
}

void MainWindow::calibrationDataTreewidgetItemExpanded(QTreeWidgetItem *item)
{
    setCategoryExpanded(item, true);
}

void MainWindow::calibrationDataTreewidgetItemCollapsed(QTreeWidgetItem *item)
{
    setCategoryExpanded(item, false);
}

void MainWindow::setCategoryExpanded(QTreeWidgetItem *item, bool expanded)
{
    const int itemIndex = ui_->calibrationDataTreeWidget->indexOfTopLevelItem(item);
    OpenCalibration *open = selectedOpenCalibration();
    if (itemIndex < 0 || open == nullptr)
    {
        return;
    }
    const QString categoryName = ui_->calibrationDataTreeWidget->topLevelItem(itemIndex)->text(0);
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

void MainWindow::closeCalibration()
{
    const QList<QTreeWidgetItem *> selected = ui_->calibrationFilesTreeWidget->selectedItems();
    const auto id = selected.isEmpty() ? std::nullopt : sessionOf(selected.at(0));
    if (!id.has_value())
    {
        return;
    }
    const int romNumber = ui_->calibrationFilesTreeWidget->indexOfTopLevelItem(selected.at(0));
    const QString romKey = fastecu::ui::sessionKeyText(*id);

    const QString windowPrefix = romKey + ",";
    for (QMdiSubWindow *w : ui_->mdiArea->subWindowList())
    {
        if (w->objectName().startsWith(windowPrefix))
        {
            ui_->mdiArea->removeSubWindow(w);
        }
    }
    delete ui_->calibrationFilesTreeWidget->takeTopLevelItem(romNumber);
    std::erase_if(calibrations_, [&id](const OpenCalibration& open) { return open.id == *id; });
    std::ignore = calibration_workspace_->Close(*id);

    if (ui_->calibrationFilesTreeWidget->topLevelItemCount() > 0)
    {
        for (int i = 0; i < ui_->calibrationFilesTreeWidget->topLevelItemCount(); i++)
        {
            ui_->calibrationFilesTreeWidget->topLevelItem(i)->setSelected(false);
        }
        int activeRom = 0;
        if (romNumber > 0)
        {
            activeRom = romNumber - 1;
        }
        QTreeWidgetItem *currentItem = ui_->calibrationFilesTreeWidget->topLevelItem(activeRom);
        currentItem->setSelected(true);
        const QModelIndex index = ui_->calibrationFilesTreeWidget->selectionModel()->currentIndex();
        emit ui_->calibrationFilesTreeWidget->clicked(index);
    }
    else
    {
        for (int i = ui_->calibrationDataTreeWidget->topLevelItemCount(); i > 0; i--)
        {
            delete ui_->calibrationDataTreeWidget->takeTopLevelItem(0);
        }
    }
    // configSession->settings().calibration_files.erase(...);
    // save_settings();
}

void MainWindow::closeCalibrationMap(QObject *obj)
{
    const QStringList mapWindowString = obj->objectName().split(",");
    const auto id = mapWindowString.isEmpty() ? std::nullopt : fastecu::ui::parseSessionKey(mapWindowString.at(0));
    QTreeWidgetItem *romItem = id.has_value() ? filesTreeItem(*id) : nullptr;
    OpenCalibration *open = id.has_value() ? openCalibration(*id) : nullptr;
    if (romItem == nullptr || open == nullptr || mapWindowString.size() < 3)
    {
        return; // the ROM was closed before its window
    }
    const QString& mapName = mapWindowString.at(2);

    for (int i = 0; i < ui_->calibrationFilesTreeWidget->topLevelItemCount(); i++)
    {
        ui_->calibrationFilesTreeWidget->topLevelItem(i)->setSelected(false);
    }
    romItem->setSelected(true);
    const QModelIndex index = ui_->calibrationFilesTreeWidget->selectionModel()->currentIndex();
    emit ui_->calibrationFilesTreeWidget->clicked(index);

    QTreeWidgetItem *item;
    for (int i = 0; i < ui_->calibrationDataTreeWidget->topLevelItemCount(); i++)
    {
        for (int j = 0; j < ui_->calibrationDataTreeWidget->topLevelItem(i)->childCount(); j++)
        {
            item = ui_->calibrationDataTreeWidget->topLevelItem(i)->child(j);
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
    closeApp();
}

void MainWindow::closeApp()
{
    qApp->exit();
}

void MainWindow::changeGaugeValues()
{
    changeLogValues(0, protocol_);
    // if (ecu_init_complete)
    //     save_logger_selection();
}

void MainWindow::changeDigitalValues()
{
    changeLogValues(1, protocol_);
}

void MainWindow::changeSwitchValues()
{
    changeLogValues(2, protocol_);
}

void MainWindow::updateLogboxes(const QString& protocolArg)
{
    int switchBoxCount = 20;
    int logBoxCount = 12;

    emit logD("Update logboxes with protocol: " + protocolArg, true, true);

    while (!ui_->switchBoxLayout->isEmpty())
    {
        QWidget *wg = ui_->switchBoxLayout->takeAt(0)->widget();
        delete wg;
    }
    while (!ui_->logBoxLayout->isEmpty())
    {
        QWidget *wg = ui_->logBoxLayout->takeAt(0)->widget();
        delete wg;
    }

    const auto key = protocolArg.toStdString();
    const auto& selection = logger_model_->Selection();
    for (std::size_t slot = 0; slot < selection.switch_ids.size(); ++slot)
    {
        const auto& id = selection.switch_ids[slot];
        const auto *item = logger_model_->SwitchDefinition(key, id);
        if (item == nullptr)
        {
            continue;
        }
        const auto name = qs(item->name);
        auto *box = log_boxes_->drawLogBoxes("switch", static_cast<int>(slot), switchBoxCount, name, name,
                                             logger_values_.SwitchValue(key, id));
        box->setAttribute(Qt::WA_TransparentForMouseEvents);
        ui_->switchBoxLayout->addWidget(box);
    }
    for (std::size_t slot = 0; slot < selection.lower_panel_ids.size(); ++slot)
    {
        const auto& id = selection.lower_panel_ids[slot];
        const auto *item = logger_model_->Parameter(key, id);
        if (item == nullptr)
        {
            continue;
        }
        const auto unit = item->conversions.empty() ? QString{} : qs(item->conversions.front().units);
        auto *box = log_boxes_->drawLogBoxes("log", static_cast<int>(slot), logBoxCount, qs(item->name), unit,
                                             logger_values_.ParameterValue(key, id));
        box->setAttribute(Qt::WA_TransparentForMouseEvents);
        ui_->logBoxLayout->addWidget(box);
    }
}

void MainWindow::updateLogboxValues(const QString& protocolArg)
{
    const auto key = protocolArg.toStdString();
    const auto& ids = logger_model_->Selection().lower_panel_ids;
    // Layout positions can differ from selection slots when IDs are unresolved.
    // Labels keep the original slot in their object name.
    for (std::size_t slot = 0; slot < ids.size(); ++slot)
    {
        const auto *item = logger_model_->Parameter(key, ids[slot]);
        if (item == nullptr)
        {
            continue;
        }
        auto *label = ui_->centralwidget->findChild<QLabel *>("log_label" + QString::number(slot));
        if (label == nullptr)
        {
            continue;
        }
        const auto unit = item->conversions.empty() ? QString{} : qs(item->conversions.front().units);
        const auto text = logger_values_.ParameterValue(key, ids[slot]);
        label->setAlignment(Qt::AlignRight);
        label->setText(text + " <font size=1px color=grey>" + unit + "</font>");
        const auto size = QGuiApplication::primaryScreen()->geometry();
        label->setFont(QFont("Arial", size.width() / 90));
    }
    delay(1);
}

void MainWindow::loadLoggerDefinition()
{
    auto& settings = config_session_->Settings();
    auto& service = services_.logger_definitions;
    const auto handle =
        service.ResolveDefinitionHandle(settings.romraider_logger_definition_file, settings.selected_log_protocol,
                                        config_session_->EffectivePaths().config_files_directory);
    if (!handle.has_value())
    {
        services_.file_action_events.Notice("Logger file: Unable to resolve logger definition file: " +
                                            handle.error().detail);
        return;
    }
    if (settings.romraider_logger_definition_file.empty() && !handle->empty())
    {
        settings.romraider_logger_definition_file = *handle;
        emit logD("Using bundled CDBG logger definition: " + qs(*handle), true, true);
    }
    auto definition = service.LoadDefinition(*handle);
    if (!definition.has_value())
    {
        services_.file_action_events.Notice("Logger file: Unable to open logger definition file '" + *handle +
                                            "' for reading: " + definition.error().detail);
        return;
    }
    logger_model_->InstallDefinition(std::move(*definition));
}

void MainWindow::loadLoggerSelection()
{
    const auto& handle = config_session_->EffectivePaths().logger_file;
    auto& service = services_.logger_definitions;
    const auto stored = service.LoadSelection(handle, ecuid_.toStdString());
    if (!stored.has_value())
    {
        services_.file_action_events.Notice("Logger file: Unable to open logger config file '" + handle +
                                            "' for reading");
        return;
    }
    // A successful read clears IDs even if no ECU entry or definition exists.
    logger_model_->SetSelection({.protocol = logger_model_->Selection().protocol});
    if (stored->has_value())
    {
        logger_model_->SetSelection(**stored);
        return;
    }
    if (logger_model_->Definition().parameters.empty())
    {
        services_.file_action_events.Notice("Logger definition file: No logger definition file selected, returning "
                                            "without initializing log parameters!");
        return;
    }
    const auto selected =
        service.LoadOrInitializeSelection(handle, ecuid_.toStdString(), logger_model_->DefaultSelection());
    if (!selected.has_value())
    {
        services_.file_action_events.Notice("Logger file: Unable to open logger config file '" + handle +
                                            "' for reading");
        return;
    }
    logger_model_->SetSelection(*selected);
}

void MainWindow::saveLoggerSelection()
{
    const auto& handle = config_session_->EffectivePaths().logger_file;
    const auto saved =
        services_.logger_definitions.SaveSelection(handle, ecuid_.toStdString(), logger_model_->Selection());
    if (!saved.has_value())
    {
        services_.file_action_events.Notice("Logger file: Unable to open logger config file '" + handle +
                                            "' for reading");
    }
}

bool MainWindow::event(QEvent *event)
{
    // Events can arrive before the constructor has bound the session.
    if (config_session_ != nullptr && (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Resize))
    {
        fastecu::config::AppConfig& settings = config_session_->Settings();
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
        saveSettings();
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

    QWidget *w = ui_->mdiArea->findChild<QWidget *>("gaugeWindow");
    if (w)
    {
        delete w;
    }
}

void MainWindow::setStatusBarLabel(bool serialConnectionState, bool ecuConnectionState, const QString& romId)
{
    QString connectionStatusString;

    QString softwareVersionString = "FastECU";
    if (ecuConnectionState)
    {
        connectionStatusString = "ECU connected";
        status_bar_connection_label_->setStyleSheet("QLabel { background-color : green; color : white; color: white;}");
    }
    else if (serialConnectionState)
    {
        connectionStatusString = "ECU not connected";
        status_bar_connection_label_->setStyleSheet(
            "QLabel { background-color : yellow; color : white; color: black;}");
    }
    else
    {
        connectionStatusString = "Serial port unavailable";
        status_bar_connection_label_->setStyleSheet("QLabel { background-color : red; color : white; color: white;}");
    }
    QString romIdString = "ECU ID: " + romId;
    QString statusBarString = softwareVersionString + " | " + connectionStatusString + " | " + romIdString;
    status_bar_connection_label_->setText(statusBarString);
}

void MainWindow::delay(int n)
{
    QTime dieTime = QTime::currentTime().addMSecs(n);
    while (QTime::currentTime() < dieTime)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

void MainWindow::addNewEcuDefinitionFile()
{
    QString filename;
    QObject *obj = sender();
    QListWidget *definitionFiles = obj->parent()->parent()->findChild<QListWidget *>("ecu_definition_files_list");

    QFileDialog openDialog;
    openDialog.setDefaultSuffix("xml");
    filename = QFileDialog::getOpenFileName(this, tr("Select definition file"),
                                            qs(config_session_->EffectivePaths().definition_files_directory),
                                            tr("ECU definition file (*.xml)"));

    if (filename.isEmpty())
    {
        QMessageBox::information(this, tr("ECU definition file"), "No file selected");
    }
    else
    {
        definitionFiles->addItem(filename);
        config_session_->Settings().romraider_definition_files.push_back(filename.toStdString());
        saveSettings();
    }
}

void MainWindow::removeEcuDefinitionFile()
{
    QObject *obj = sender();
    QListWidget *definitionFiles = obj->parent()->parent()->findChild<QListWidget *>("ecu_definition_files_list");
    QList<QModelIndex> index = definitionFiles->selectionModel()->selectedIndexes();

    int row = 0;
    for (auto i = static_cast<int>(index.length()) - 1; i >= 0; i--)
    {
        row = index.at(i).row();
        definitionFiles->model()->removeRow(row);
        std::vector<std::string>& files = config_session_->Settings().romraider_definition_files;
        if (static_cast<std::size_t>(row) < files.size())
        {
            files.erase(files.begin() + row);
        }
    }
    if (!index.empty())
    {
        saveSettings();
    }
}

void MainWindow::addNewLoggerDefinitionFile()
{
}

void MainWindow::removeLoggerDefinitionFile()
{
}

QString MainWindow::parseMessageToHex(const QByteArray& received)
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
    if (target == net_splash_)
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

template <typename FlashClass> FlashClass *MainWindow::connectSignalsAndRunModule(FlashClass *object)
{
    // To successfully connect an overloaded signal,
    // one should provide a suitable template parameter to connect()
    QObject::connect<void (FlashClass::*)(QString)>(object, &FlashClass::external_logger, this,
                                                    &MainWindow::externalLogger);
    QObject::connect<void (FlashClass::*)(int)>(object, &FlashClass::external_logger, this,
                                                &MainWindow::externalLoggerSetProgressbarValue);

    // If signal is not overloaded, QObject::connect<> template will deduce type automatically
    QObject::connect(object, &FlashClass::logE, log_channel_, &fastecu::ui::LogChannel::logE);
    QObject::connect(object, &FlashClass::logW, log_channel_, &fastecu::ui::LogChannel::logW);
    QObject::connect(object, &FlashClass::logI, log_channel_, &fastecu::ui::LogChannel::logI);
    QObject::connect(object, &FlashClass::logD, log_channel_, &fastecu::ui::LogChannel::logD);

    object->run();
    return object;
}

// External logger slot for string messages
void MainWindow::externalLogger(const QString& message)
{
    emit logD(Q_FUNC_INFO, true, false);
    emit logD(" " + message, false, true);
    emit remote_peer_->logWindowMessage(message);
}

// External progress bar slot
void MainWindow::externalLoggerSetProgressbarValue(int value)
{
    emit logD(Q_FUNC_INFO, true, false);
    emit logD(" " + QString::number(value), false, true);
    emit remote_peer_->progress(value);
}

void MainWindow::sendMessageToLogWindow(const QString& msg)
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
        emit logD("Found dataTerminalWindow", true, true);
        QTextEdit *textEdit = dataTerminalWindow->findChild<QTextEdit *>("text_edit");
        if (textEdit)
        {
            emit logD("Found dataTerminalWindow->textEdit", true, true);
            textEdit->insertPlainText(msg);
            textEdit->ensureCursorVisible();
        }
    }
}

void MainWindow::updateVbatt()
{
    if (connection_coordinator_->identifying())
    {
        return;
    }
    const std::optional<unsigned long> reading = connection_->BatteryMillivolts();
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
            emit logD(vBattText, true, true);
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
            emit logD(vBattText, true, true);
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
            emit logD(vBattText, true, true);
        }
    }
    QDialog *dataTerminalWindow = this->findChild<QDialog *>("DataTerminalWindow");
    if (dataTerminalWindow)
    {
        emit logD("Found dataTerminalWindow", true, true);
        QLabel *vBattLabel = dataTerminalWindow->findChild<QLabel *>("vBattLabel");
        if (vBattLabel)
        {
            emit logD("Found dataTerminalWindow->vBattLabel", true, true);
            QString vBattText = QString("Battery: %1").arg(static_cast<double>(vBatt) / 1000.0, 0, 'f', 3) + " V";
            vBattLabel->setText(vBattText);
            emit logD(vBattText, true, true);
        }
    }
}

void MainWindow::setupLoggingEngine()
{
    logging_engine_ = &services_.logging_engine;

    connect(logging_engine_, &fastecu::desktop::logging::LoggingEngine::valuesUpdated, this,
            &MainWindow::handleLoggingValuesUpdated);
    connect(logging_engine_, &fastecu::desktop::logging::LoggingEngine::sessionEnded, this,
            &MainWindow::handleLoggingSessionEnded);
}

void MainWindow::handleLoggingValuesUpdated(const QVector<fastecu::logging::LogSample>& samples)
{
    if (!active_logging_snapshot_)
    {
        return;
    }
    for (const auto& sample : samples)
    {
        // Re-checked per sample: LOG_E below can reach a slot that ends the session.
        if (!active_logging_snapshot_)
        {
            return;
        }
        const auto applied =
            fastecu::desktop::logging::ApplyLogSample(*active_logging_snapshot_, sample, logger_values_);
        if (!applied)
        {
            emit logE(QString::fromStdString(applied.error().detail), true, true);
        }
    }
    updateLogboxValues(active_log_value_protocol_filter_);
    logToFile();
}

void MainWindow::restoreLoggingUiState()
{
    active_logging_snapshot_.reset();
    logging_state_ = false;
    log_params_request_started_ = false;
    ui_->actionToggleRealtime->setChecked(false);
}

void MainWindow::handleLoggingSessionEnded(fastecu::desktop::logging::SessionEndReason reason, const QString& message)
{
    restoreLoggingUiState();

    if (reason == fastecu::desktop::logging::SessionEndReason::kStoppedByUser)
    {
        return;
    }

    if (reason == fastecu::desktop::logging::SessionEndReason::kAdapterDisconnected)
    {
        QMessageBox::warning(this, tr("Logging"), "Logging adapter disconnected: " + message);
    }
    else if (reason == fastecu::desktop::logging::SessionEndReason::kHandshakeFailed)
    {
        QMessageBox::warning(this, tr("Logging"), "Unable to start logging: " + message);
    }
    else if (reason == fastecu::desktop::logging::SessionEndReason::kRuntimeFailed)
    {
        QMessageBox::warning(this, tr("Logging"), "Logging stopped: " + message);
    }
}
