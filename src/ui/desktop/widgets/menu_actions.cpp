#include "src/ui/desktop/widgets/mainwindow.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/ui/desktop/config_fields.h"
#include "src/backend/calibration/map_edit.h"
#include "src/backend/calibration/session/numeric_copy_use_case.h"
#include "src/backend/calibration/session/numeric_edit_use_case.h"
#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"
#include "src/ui/desktop/calibration/map_edit_adapter.h"
#include "ui_mainwindow.h"

#include "src/backend/definition/mappack_converter.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSaveFile>
#include <QAction>
#include <QPointer>
#include <QKeySequence>

#include <algorithm>
#include <array>
#include <chrono>
#include <tuple>
#include <utility>
#include <variant>
#include <optional>
#include "src/backend/diagnostics/ssm_identify.h"
#include "src/backend/ports/event_sink.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/ui/desktop/connection/connection_coordinator.h"

namespace
{
// The log transport's identification exchange. Raw CAN never identified: its
// legacy branch tested a protocol value no configuration sets.
std::optional<fastecu::diagnostics::SsmVariant> ssm_variant_for_transport(const QString& transport)
{
    if (transport == "SSM")
    {
        return fastecu::diagnostics::SsmVariant::Ssm1;
    }
    if (transport == "K-Line")
    {
        return fastecu::diagnostics::SsmVariant::KlineSsm2;
    }
    if (transport == "iso15765")
    {
        return fastecu::diagnostics::SsmVariant::Iso15765Uds;
    }
    return std::nullopt;
}
void show_no_change(QWidget *parent, fastecu::calibration::NoChangeReason reason)
{
    using fastecu::calibration::NoChangeReason;
    QString message;
    switch (reason)
    {
    case NoChangeReason::BelowStorageResolution:
        message = "The requested change is below storage resolution; no stored values changed.";
        break;
    case NoChangeReason::DefinitionLimit:
        message = "A definition limit was reached; no stored values changed.";
        break;
    case NoChangeReason::MultipleCauses:
        message = "Storage resolution and definition limits prevented changes; no stored values changed.";
        break;
    case NoChangeReason::None:
    case NoChangeReason::Unchanged:
        message = "No stored values changed.";
        break;
    }
    QMessageBox::information(parent, "Map edit", message);
}

// Runs one numeric edit against `window`'s map. Windows without a numeric
// selection and not-applicable outcomes return silently; after a change or a
// no-op, the window is refreshed from current bytes.
void run_numeric_edit(QWidget *parent, const QString& title, fastecu::calibration::CalibrationWorkspace& workspace,
                      QMdiSubWindow *window, fastecu::calibration::NumericEditOperation operation)
{
    namespace calibration = fastecu::calibration;
    const auto id = fastecu::ui::parse_map_window_id(window);
    if (!id.has_value() || id->map_number < 0)
    {
        return;
    }
    const auto *session = workspace.find(id->session);
    if (session == nullptr)
    {
        return;
    }
    const auto selection = fastecu::ui::selected_numeric_target(window, *session, id->map_number);
    if (!selection.has_value())
    {
        return;
    }
    const auto outcome =
        calibration::apply_numeric_edit(workspace, {.session = id->session,
                                                    .map_index = static_cast<std::size_t>(id->map_number),
                                                    .selection = *selection,
                                                    .operation = std::move(operation)});
    if (!outcome.has_value())
    {
        QMessageBox::warning(parent, title, QString::fromStdString(outcome.error().detail));
        return;
    }
    if (std::holds_alternative<calibration::NumericEditNotApplicable>(*outcome))
    {
        return;
    }
    if (auto *map = qobject_cast<CalibrationMaps *>(window->widget()); map != nullptr)
    {
        map->refresh();
    }
    if (const auto *unchanged = std::get_if<calibration::NumericEditUnchanged>(&*outcome); unchanged != nullptr)
    {
        show_no_change(parent, unchanged->reason);
    }
}
} // namespace

void MainWindow::connect_menu_actions()
{
    using fastecu::calibration::IncrementStep;
    using fastecu::calibration::InterpolationMode;

    // Lambdas, not member pointers: QAction::triggered carries a bool that
    // these handlers do not take.
    connect(ui->actionOpenCalibration, &QAction::triggered, this, [this] { open_calibration_file(nullptr); });
    connect(ui->actionSaveCalibration, &QAction::triggered, this, [this] { save_calibration_file(); });
    connect(ui->actionSaveCalibrationAs, &QAction::triggered, this, [this] { save_calibration_file_as(); });
    connect(ui->actionCloseCalibration, &QAction::triggered, this, [this] { close_calibration(); });
    connect(ui->actionQuit, &QAction::triggered, this, [this] { close_app(); });
    connect(ui->actionCopy, &QAction::triggered, this, [this] { copy_value(); });
    connect(ui->actionPaste, &QAction::triggered, this, [this] { paste_value(); });
    connect(ui->actionSettings, &QAction::triggered, this, [this] { show_preferences_window(); });
    connect(ui->actionCoarseIncrement, &QAction::triggered, this, [this] { inc_dec_value(IncrementStep::CoarseUp); });
    connect(ui->actionCoarseDecrement, &QAction::triggered, this, [this] { inc_dec_value(IncrementStep::CoarseDown); });
    connect(ui->actionFineIncrement, &QAction::triggered, this, [this] { inc_dec_value(IncrementStep::FineUp); });
    connect(ui->actionFineDecrement, &QAction::triggered, this, [this] { inc_dec_value(IncrementStep::FineDown); });
    connect(ui->actionSetValue, &QAction::triggered, this, [this] { set_value(); });
    connect(ui->actionInterpolateHorizontal, &QAction::triggered, this,
            [this] { interpolate_value(InterpolationMode::Horizontal); });
    connect(ui->actionInterpolateVertical, &QAction::triggered, this,
            [this] { interpolate_value(InterpolationMode::Vertical); });
    connect(ui->actionInterpolateBidirectional, &QAction::triggered, this,
            [this] { interpolate_value(InterpolationMode::Bidirectional); });
    connect(ui->actionConnectToEcu, &QAction::triggered, this, [this] { connect_to_ecu(); });
    connect(ui->actionDisconnectFromEcu, &QAction::triggered, this, [this] { disconnect_from_ecu(); });
    connect(ui->actionToggleRealtime, &QAction::triggered, this, [this] { toggle_realtime(); });
    connect(ui->actionLogToFile, &QAction::triggered, this, [this] { toggle_log_to_file(); });
    connect(ui->actionReadRomFromEcu, &QAction::triggered, this, [this] { start_ecu_operations("read"); });
    connect(ui->actionTestWriteRomToEcu, &QAction::triggered, this, [this] { start_ecu_operations("test_write"); });
    connect(ui->actionWriteRomToEcu, &QAction::triggered, this, [this] { start_ecu_operations("write"); });
    connect(ui->actionSetLogViews, &QAction::triggered, this, [this] { change_gauge_values(); });
    connect(ui->actionDtcWindow, &QAction::triggered, this, [this] { show_dtc_window(); });
    connect(ui->actionHexEditor, &QAction::triggered, this, [this] { show_hex_editor(); });
    connect(ui->actionTerminal, &QAction::triggered, this, [this] { show_terminal_window(); });
    connect(ui->actionBiuCommunication, &QAction::triggered, this, [this] { show_subaru_biu_window(); });
    connect(ui->actionGetKey, &QAction::triggered, this, [this] { show_subaru_get_key_window(); });
    connect(ui->actionWinolsCsvToRomRaiderXml, &QAction::triggered, this, [this] { winols_csv_to_romraider_xml(); });
    connect(ui->actionAbout, &QAction::triggered, this, [this] { show_about_dialog(); });
}

namespace
{
// Qt's primary binding for a standard key, or `fallback` where the platform
// has none (Save As and Quit on Windows). Designer stores only literal
// sequences, so these are set here rather than in the .ui.
void set_standard_shortcut(QAction *action, QKeySequence::StandardKey key, const char *fallback)
{
    const QKeySequence standard{key};
    action->setShortcut(standard.isEmpty() ? QKeySequence{QString::fromLatin1(fallback)} : standard);
}
} // namespace

void MainWindow::apply_standard_shortcuts()
{
    set_standard_shortcut(ui->actionOpenCalibration, QKeySequence::Open, "Ctrl+O");
    set_standard_shortcut(ui->actionSaveCalibration, QKeySequence::Save, "Ctrl+S");
    set_standard_shortcut(ui->actionSaveCalibrationAs, QKeySequence::SaveAs, "Ctrl+Shift+S");
    set_standard_shortcut(ui->actionCopy, QKeySequence::Copy, "Ctrl+C");
    set_standard_shortcut(ui->actionPaste, QKeySequence::Paste, "Ctrl+V");
    set_standard_shortcut(ui->actionQuit, QKeySequence::Quit, "Ctrl+Q");
}

void MainWindow::show_about_dialog()
{
    QMessageBox::information(this, tr("FastECU"),
                             "FastECU is open source tuning software for Subaru ECUs,\n"
                             "TCUs and also modifying BIU and ECUs of other car makes.\n"
                             "\n"
                             "This is beta test version for read and write ROMs via\n"
                             "K-Line and CAN connection with Open Port 2.0 or generic\n"
                             "OBD2 cable. Software is tested in Win7/Win10 32/64bit\n"
                             "and Linux amd64 and aarch64 platforms.\n"
                             "\n"
                             "There WILL be bugs and things that don't work. Be patient\n"
                             "with new versions relesed.\n"
                             "\n"
                             "All liability lies with the user. We are not responsible any\n"
                             "harm, laws broken or bricked ECUs that can follow for using\n"
                             "this software.\n"
                             "\n"
                             "\n"
                             "Huge thanks to following:\n"
                             "\n"
                             "fenugrec - author of nisprog software\n"
                             "rimwall - modifier of nisprog kernels for Subaru use\n"
                             "SergArb - testing and software development\n"
                             "alesv - testing and software development\n"
                             "jimihimisimi - testing and software development\n"
                             "\n"
                             "...and to all of you who had support software development by\n"
                             "donating! All, even the smallest amount of donates are welcome!\n");
}

void MainWindow::inc_dec_value(fastecu::calibration::IncrementStep step)
{
    run_numeric_edit(this, tr("Set value"), *calibrationWorkspace, ui->mdiArea->activeSubWindow(),
                     fastecu::calibration::IncrementEdit{.step = step});
}

void MainWindow::set_value()
{
    QMdiSubWindow *w = ui->mdiArea->activeSubWindow();
    const auto id = fastecu::ui::parse_map_window_id(w);
    if (!id || calibrationWorkspace->find(id->session) == nullptr || w->findChild<QTableWidget *>() == nullptr)
    {
        return;
    }

    // The dialog may outlive the window, its session, or the active
    // selection; only the original window's identity is carried across it.
    const QPointer<QMdiSubWindow> original_window(w);
    bool accepted = false;
    QString text =
        QInputDialog::getText(this, tr("QInputDialog::getText()"),
                              tr("Set value: 20 | -20 | x+20 | x-20 | x*20 | x/20"), QLineEdit::Normal, "", &accepted);
    text.replace(",", ".");
    if (!accepted || text.isEmpty() || original_window.isNull())
    {
        return;
    }
    run_numeric_edit(this, tr("Set value"), *calibrationWorkspace, original_window.data(),
                     fastecu::calibration::AssignmentEdit{.expression = text.toStdString()});
}

void MainWindow::interpolate_value(fastecu::calibration::InterpolationMode mode)
{
    run_numeric_edit(this, tr("Set value"), *calibrationWorkspace, ui->mdiArea->activeSubWindow(),
                     fastecu::calibration::InterpolationEdit{.mode = mode});
}

// Copies the selected body or axis values at full precision, in the plain
// decimals Paste accepts. Selections that are not a numeric target copy nothing.
// A selection holding a cell with no valid value is refused, since the
// clipboard text could not be pasted back.
void MainWindow::copy_value()
{
    namespace calibration = fastecu::calibration;
    QMdiSubWindow *window = ui->mdiArea->activeSubWindow();
    const auto id = fastecu::ui::parse_map_window_id(window);
    if (!id.has_value() || id->map_number < 0)
    {
        return;
    }
    const auto *session = calibrationWorkspace->find(id->session);
    if (session == nullptr)
    {
        return;
    }
    const auto selection = fastecu::ui::selected_numeric_target(window, *session, id->map_number);
    if (!selection.has_value())
    {
        return;
    }
    const auto copied = calibration::copy_numeric_values(
        *calibrationWorkspace,
        {.session = id->session, .map_index = static_cast<std::size_t>(id->map_number), .selection = *selection});
    if (!copied.has_value())
    {
        QMessageBox::warning(this, tr("Copy value"), QString::fromStdString(copied.error().detail));
        return;
    }
    if (const auto *text = std::get_if<calibration::NumericCopyText>(&*copied); text != nullptr)
    {
        QApplication::clipboard()->setText(QString::fromStdString(text->text));
    }
    else if (const auto *invalid = std::get_if<calibration::NumericCopyInvalidCell>(&*copied); invalid != nullptr)
    {
        QMessageBox::warning(this, tr("Copy value"),
                             tr("Nothing was copied: the cell at row %1, column %2 of the selected values has no valid "
                                "value (%3). Select only valid cells.")
                                 .arg(invalid->row + 1)
                                 .arg(invalid->col + 1)
                                 .arg(QString::fromStdString(invalid->detail)));
    }
}

// Pasting onto a selected axis edits that axis, and a `y_size == 1` map pastes
// at its correct 0-based column (legacy applied an unconditional `-1` column
// offset). The backend validates every supplied cell before clipping at the
// target run's edges.
void MainWindow::paste_value()
{
    run_numeric_edit(
        this, tr("Paste value"), *calibrationWorkspace, ui->mdiArea->activeSubWindow(),
        fastecu::calibration::PasteEdit{.rows = fastecu::ui::split_paste_rows(QApplication::clipboard()->text())});
}

void MainWindow::connect_to_ecu(std::function<void(bool)> on_done)
{
    connection_coordinator_->cancel();
    const bool had_logging_run = loggingEngine->isRunning() || activeLoggingSnapshot.has_value();
    if (loggingEngine->isRunning())
    {
        loggingEngine->stop();
    }
    // A checked Logging action can be a request still waiting for identification.
    if (had_logging_run)
    {
        restoreLoggingUiState();
    }
    else
    {
        end_logging_csv();
    }
    ecuid.clear();
    ecu_init_complete = false;
    set_status_bar_label(false, false, "");
    connection->reset();

    qDebug() << "Opening interface, please wait...";
    open_serial_port();
    if (!connection->is_open())
    {
        QMessageBox::warning(this, tr("Serial port"), "Could not open interface!");
        if (on_done)
        {
            on_done(false);
        }
        return;
    }
    serial_port_list->setDisabled(true);
    refresh_serial_port_list->setDisabled(true);

    const std::optional<fastecu::diagnostics::SsmVariant> variant =
        selected_vehicle().make == "Subaru"
            ? ssm_variant_for_transport(fastecu::ui::qs(configSession->settings().selected_log_transport))
            : std::nullopt;
    if (!variant.has_value())
    {
        // ecu_init did nothing for other makes and for raw CAN; the legacy
        // loop spent 2.5 s on it and then disconnected.
        disconnect_from_ecu();
        if (on_done)
        {
            on_done(true);
        }
        return;
    }

    qDebug() << "Initialising ECU, please wait...";
    connection_coordinator_->begin(
        fastecu::diagnostics::SsmIdentifyRequest{*variant, ecu_radio_button->isChecked()
                                                               ? fastecu::diagnostics::SsmTarget::Ecu
                                                               : fastecu::diagnostics::SsmTarget::Tcu},
        std::move(on_done));
}

void MainWindow::ConnectionPresentation::set_controls_locked(bool locked)
{
    window_.log_transport_list->setEnabled(!locked);
    window_.ecu_radio_button->setEnabled(!locked);
    window_.tcu_radio_button->setEnabled(!locked);
    window_.ui->actionConnectToEcu->setEnabled(!locked);
    window_.ui->actionToggleRealtime->setEnabled(!locked);
}

void MainWindow::ConnectionPresentation::set_port_selector_enabled(bool enabled)
{
    window_.serial_port_list->setEnabled(enabled);
    window_.refresh_serial_port_list->setEnabled(enabled);
}

void MainWindow::ConnectionPresentation::identified(const fastecu::ui::IdentifyOutcome& outcome)
{
    window_.loggerModel->reset_support();
    window_.ecu_init_complete = true;
    window_.ecuid = QString::fromStdString(outcome.ecu_id);
    emit window_.LOG_D("ECU ID: " + window_.ecuid, true, true);
    window_.set_status_bar_label(true, !window_.ecuid.isEmpty(), window_.ecuid);
    if (!outcome.init_response.empty())
    {
        window_.parse_log_value_list(bytes::toQByteArray(outcome.init_response), "SSM");
    }
}

void MainWindow::ConnectionPresentation::identification_failed(const fastecu::ui::IdentifyOutcome& outcome)
{
    emit window_.LOG_W("ECU identification failed: " + QString::fromStdString(outcome.error_detail), true, true);
    window_.disconnect_from_ecu();
}

void MainWindow::disconnect_from_ecu()
{
    connection_coordinator_->cancel();
    qDebug() << "Disconnecting...";
    ecuid.clear();
    ecu_init_complete = false;
    set_status_bar_label(false, false, "");
    connection->return_to_idle();

    serial_port_list->setEnabled(true);
    refresh_serial_port_list->setEnabled(true);
}

void MainWindow::ecu_definition_manager()
{
    QDialog *definitions_manager_dialog = new QDialog;
    definitions_manager_dialog->setObjectName("ecu_definition_manager_dialog");
    definitions_manager_dialog->setFixedWidth(640);
    definitions_manager_dialog->setFixedHeight(240);
    definitions_manager_dialog->setWindowModality(Qt::ApplicationModal);
    definitions_manager_dialog->resize(800, 600);
    definitions_manager_dialog->setWindowTitle("ECU definition manager");
    definitions_manager_dialog->setAttribute(Qt::WA_DeleteOnClose);

    QVBoxLayout *definitions_manager_layout = new QVBoxLayout;
    definitions_manager_dialog->setLayout(definitions_manager_layout);

    QListWidget *definition_files = new QListWidget;
    definition_files->setObjectName("ecu_definition_files_list");
    definition_files->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const std::string& file : configSession->settings().romraider_definition_files)
    {
        new QListWidgetItem(fastecu::ui::qs(file), definition_files);
    }
    definitions_manager_layout->addWidget(definition_files);

    QWidget *definitions_manager_widget = new QWidget;
    QHBoxLayout *definitions_manager_buttons = new QHBoxLayout;
    definitions_manager_layout->addWidget(definitions_manager_widget);
    definitions_manager_widget->setLayout(definitions_manager_buttons);

    QPushButton *add_new_file = new QPushButton("Add new file");
    QPushButton *remove_file = new QPushButton("Remove file");
    QPushButton *close = new QPushButton("Close");
    QSpacerItem *spacer = new QSpacerItem(20, 20, QSizePolicy::MinimumExpanding, QSizePolicy::Minimum);

    definitions_manager_buttons->addWidget(add_new_file);
    definitions_manager_buttons->addWidget(remove_file);
    definitions_manager_buttons->addSpacerItem(spacer);
    definitions_manager_buttons->addWidget(close);

    connect(add_new_file, SIGNAL(clicked()), this, SLOT(add_new_ecu_definition_file()));
    connect(remove_file, SIGNAL(clicked()), this, SLOT(remove_ecu_definition_file()));
    connect(close, SIGNAL(clicked()), definitions_manager_dialog, SLOT(close()));
    definitions_manager_dialog->exec();
}

void MainWindow::logger_definition_manager()
{
}

void MainWindow::set_realtime_state(bool state)
{
    ui->actionToggleRealtime->setChecked(state);
}

void MainWindow::toggle_realtime()
{
    using namespace std::chrono_literals;

    logging_state = ui->actionToggleRealtime->isChecked();

    if (logging_state)
    {
        qDebug() << "Start datalog";
        if (!ecu_init_complete)
        {
            connect_to_ecu(
                [this](bool connected)
                {
                    if (!connected)
                    {
                        restoreLoggingUiState();
                        QMessageBox::information(this, tr("ECU connection"), "Unable to connect to ECU");
                        return;
                    }
                    continue_start_logging();
                });
            return;
        }
        continue_start_logging();
    }
    else
    {
        qDebug() << "Stop datalog";
        end_logging_csv();

        loggingEngine->stop();

        // disconnect_from_ecu();
    }
}

void MainWindow::continue_start_logging()
{
    using namespace std::chrono_literals;

    logging_state = true;

    fastecu::desktop::logging::LogSessionConfig config;
    fastecu::logging::LoggingProtocolId protocol_id;
    fastecu::logging::LoggingPolicy logging_policy{};
    if (configSession->settings().selected_log_protocol == "MUT_DMA")
    {
        config.protocolId = "MUT_DMA";
        activeLogValueProtocolFilter = "MUT_DMA";
        protocol_id = fastecu::logging::LoggingProtocolId::MutDma;
        logging_policy = {.poll_timeout = 50ms,
                          .car_silence_miss_threshold = 20,
                          .reconnect_attempt_threshold = 100,
                          .reconnect_retry_period = 20};
    }
    else if (configSession->settings().selected_log_protocol == "CDBG")
    {
        config.protocolId = "CDBG";
        activeLogValueProtocolFilter = "CDBG";
        protocol_id = fastecu::logging::LoggingProtocolId::Cdbg;
        logging_policy = {.poll_timeout = 50ms,
                          .car_silence_miss_threshold = 20,
                          .reconnect_attempt_threshold = 100,
                          .reconnect_retry_period = 20};
    }
    else
    {
        config.protocolId = "SSM";
        activeLogValueProtocolFilter = protocol;
        protocol_id = fastecu::logging::LoggingProtocolId::Ssm;
        logging_policy = {.poll_timeout = 300ms,
                          .car_silence_miss_threshold = 10,
                          .reconnect_attempt_threshold = 30,
                          .reconnect_retry_period = 10};
    }

    const auto target =
        ecu_radio_button->isChecked() ? fastecu::logging::LoggingTarget::Ecu : fastecu::logging::LoggingTarget::Tcu;
    auto snapshot = fastecu::desktop::logging::make_desktop_logging_snapshot(
        *loggerModel, protocol_id, activeLogValueProtocolFilter, logging_policy, target);
    if (!snapshot.has_value())
    {
        emit LOG_E("Logging session failed to start: " + QString::fromStdString(snapshot.error().detail), true, true);
        restoreLoggingUiState();
        QMessageBox notice(this);
        notice.setIcon(QMessageBox::Information);
        notice.setWindowTitle(tr("Logging"));
        notice.setTextFormat(Qt::PlainText);
        notice.setText(tr("Unable to start logging:\n") + QString::fromStdString(snapshot.error().detail));
        notice.setStandardButtons(QMessageBox::Ok);
        notice.exec();
        return;
    }

    loggerValues.begin_run(*snapshot);
    activeLoggingSnapshot.emplace(*snapshot);
    logging_csv_failed_ = false;
    last_logging_csv_path_.clear();
    log_file_timer->start();
    update_logboxes(activeLogValueProtocolFilter);
    begin_logging_csv();
    if (!activeLoggingSnapshot)
    {
        return;
    }
    const auto started = loggingEngine->start(config, std::move(*snapshot));
    if (!started.has_value())
    {
        restoreLoggingUiState();
        QMessageBox::information(this, tr("Logging"), "Unable to start logging");
        return;
    }
}

void MainWindow::toggle_log_to_file()
{
    write_datalog_to_file = ui->actionLogToFile->isChecked();
    if (!write_datalog_to_file)
    {
        end_logging_csv();
    }
    else
    {
        logging_csv_failed_ = false;
        begin_logging_csv();
    }
}

void MainWindow::show_dtc_window()
{
    connection_coordinator_->cancel();
    const QString port = selected_serial_port();
    if (port.isEmpty())
    {
        QMessageBox::warning(this, tr("Serial port"), "No serial port selected!");
        return;
    }
    connection->reset();
    ecuid.clear();
    ecu_init_complete = false;
    connection->select_port(port);

    emit LOG_D("Starting DTC operations", true, true);

    fastecu::diagnostics::SerialDiagnosticLink link(&connection->facade());
    DtcOperations dtcOperations(link, this);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_E, log_channel, &fastecu::ui::LogChannel::LOG_E);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_W, log_channel, &fastecu::ui::LogChannel::LOG_W);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_I, log_channel, &fastecu::ui::LogChannel::LOG_I);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_D, log_channel, &fastecu::ui::LogChannel::LOG_D);

    dtcOperations.exec();

    emit LOG_D("DTC operations stopped", true, true);
}

void MainWindow::show_hex_editor()
{
    emit LOG_D("Show hex editor", true, true);

    // The window shows itself from its own constructor and is parented to
    // MainWindow, so without WA_DeleteOnClose every invocation left a
    // closed-but-alive HexEdit -- and its copy of the ROM data -- alive
    // until MainWindow was destroyed. HexEdit::closeEvent() ignores the
    // event when the user cancels a save prompt, and Qt only deletes on an
    // accepted close, so cancelling still keeps the window. It holds its own
    // copy of the image, so it may outlive the ROM it was opened from.
    if (auto *session = selected_calibration(); session != nullptr)
    {
        HexEdit *hexEdit = new HexEdit(bytes::toQByteArray(session->rom()),
                                       QString::fromStdString(session->source().display_name), this);
        hexEdit->setAttribute(Qt::WA_DeleteOnClose);
    }
}

void MainWindow::show_preferences_window()
{
    Settings settings(*configSession);
    settings.exec();
}

void MainWindow::show_subaru_biu_window()
{
    connection_coordinator_->cancel();
    const QString port = selected_serial_port();
    if (port.isEmpty())
    {
        QMessageBox::warning(this, tr("Serial port"), "No serial port selected!");
        return;
    }
    ecuid.clear();
    ecu_init_complete = false;
    connection->select_port(port);

    fastecu::diagnostics::SerialDiagnosticLink link(&connection->facade());
    const auto opened = link.open(fastecu::diagnostics::KlineLinkConfig{
        .header = fastecu::diagnostics::KlineHeader::None, .iso14230_connection = true, .baud = 10400});
    if (opened.has_value())
    {
        // The legacy BIU path opened through open_serial_port, which also
        // remembered the port; keep that.
        remember_opened_port(port, connection->opened_port());
    }
    set_status_bar_label(opened.has_value(), false, "");
    if (!opened.has_value())
    {
        emit LOG_E("BIU: could not open the interface: " + QString::fromStdString(opened.error().detail), true, true);
    }

    BiuOperationsSubaru biuOperationsSubaru(link, this);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_E, log_channel, &fastecu::ui::LogChannel::LOG_E);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_W, log_channel, &fastecu::ui::LogChannel::LOG_W);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_I, log_channel, &fastecu::ui::LogChannel::LOG_I);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_D, log_channel, &fastecu::ui::LogChannel::LOG_D);

    biuOperationsSubaru.exec();

    emit LOG_D("BIU stopped", true, true);

    std::ignore = link.set_header(fastecu::diagnostics::KlineHeader::None);
}

void MainWindow::show_terminal_window()
{
    connection_coordinator_->cancel();
    const QString port = selected_serial_port();
    if (port.isEmpty())
    {
        QMessageBox::warning(this, tr("Serial port"), "No serial port selected!");
        return;
    }
    connection->select_port(port);
    fastecu::diagnostics::SerialDiagnosticLink link(&connection->facade());
    DataTerminal hexCommander(link, this);
    QObject::connect(&hexCommander, &DataTerminal::LOG_E, log_channel, &fastecu::ui::LogChannel::LOG_E);
    QObject::connect(&hexCommander, &DataTerminal::LOG_W, log_channel, &fastecu::ui::LogChannel::LOG_W);
    QObject::connect(&hexCommander, &DataTerminal::LOG_I, log_channel, &fastecu::ui::LogChannel::LOG_I);
    QObject::connect(&hexCommander, &DataTerminal::LOG_D, log_channel, &fastecu::ui::LogChannel::LOG_D);

    hexCommander.exec();
}

void MainWindow::show_subaru_get_key_window()
{

    GetKeyOperationsSubaru getKeyOperationsSubaru(this);
    getKeyOperationsSubaru.exec();
}

void MainWindow::winols_csv_to_romraider_xml()
{
    const auto source = QFileDialog::getOpenFileName(this, tr("Select MapPack CSV file"), {}, tr("CSV file (*.csv)"));
    if (source.isEmpty())
    {
        return;
    }
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, tr("MapPack CSV file"), input.errorString());
        return;
    }
    const auto csv = input.readAll();
    if (input.error() != QFileDevice::NoError)
    {
        QMessageBox::warning(this, tr("MapPack CSV file"), input.errorString());
        return;
    }
    // Preserve the legacy filename convention: remove the four-character prefix.
    const auto ecu_id = QFileInfo(source).fileName().section('.', 0, 0).mid(4).toUtf8();
    const auto xml = fastecu::definition::convert_mappack_csv(std::string_view(csv.constData(), csv.size()),
                                                              std::string_view(ecu_id.constData(), ecu_id.size()));
    if (!xml.has_value())
    {
        QMessageBox::warning(this, tr("MapPack CSV file"), QString::fromStdString(xml.error().detail));
        return;
    }
    auto destination =
        QFileDialog::getSaveFileName(this, tr("Select RomRaider definition file"), {}, tr("XML file (*.xml)"));
    if (destination.isEmpty())
    {
        return;
    }
    if (!destination.endsWith(".xml", Qt::CaseInsensitive))
    {
        destination += ".xml";
    }
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(xml->data(), static_cast<qint64>(xml->size())) != static_cast<qint64>(xml->size()) ||
        !output.commit())
    {
        QMessageBox::warning(this, tr("RomRaider XML file"), output.errorString());
    }
}

void MainWindow::set_maptablewidget_items()
{
    auto *window = ui->mdiArea->activeSubWindow();
    const auto id = fastecu::ui::parse_map_window_id(window);
    if (!id.has_value() || calibrationWorkspace->find(id->session) == nullptr)
    {
        return;
    }
    if (auto *map = qobject_cast<CalibrationMaps *>(window->widget()); map != nullptr)
    {
        map->refresh();
    }
}
