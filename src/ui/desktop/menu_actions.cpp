#include "mainwindow.h"
#include "src/algorithms/menu/menu_command.h"
#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/backend/calibration/map_edit.h"
#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"
#include "src/ui/desktop/calibration/map_edit_adapter.h"
#include "ui_mainwindow.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

void MainWindow::menu_action_triggered(const QString& action)
{
    switch (menu_command_from_id(action.toStdString()))
    {
    case MenuCommand::New:
        qDebug() << action;
        break;
    case MenuCommand::OpenCalibration:
        open_calibration_file(nullptr);
        break;
    case MenuCommand::SaveCalibration:
        save_calibration_file();
        break;
    case MenuCommand::SaveCalibrationAs:
        save_calibration_file_as();
        break;
    case MenuCommand::CloseCalibration:
        close_calibration();
        break;
    case MenuCommand::Quit:
        close_app();
        break;
    case MenuCommand::Undo:
        qDebug() << action;
        break;
    case MenuCommand::Redo:
        qDebug() << action;
        break;
    case MenuCommand::Copy:
        copy_value();
        break;
    case MenuCommand::Paste:
        paste_value();
        break;
    case MenuCommand::WinolsCsvToRomRaiderXml:
        winols_csv_to_romraider_xml();
        break;
    case MenuCommand::Settings:
        show_preferences_window();
        break;
    case MenuCommand::FineIncrement:
        inc_dec_value(fastecu::calibration::IncrementStep::FineUp);
        break;
    case MenuCommand::FineDecrement:
        inc_dec_value(fastecu::calibration::IncrementStep::FineDown);
        break;
    case MenuCommand::CoarseIncrement:
        inc_dec_value(fastecu::calibration::IncrementStep::CoarseUp);
        break;
    case MenuCommand::CoarseDecrement:
        inc_dec_value(fastecu::calibration::IncrementStep::CoarseDown);
        break;
    case MenuCommand::SetValue:
        set_value();
        break;
    case MenuCommand::InterpolateHorizontal:
        interpolate_value(fastecu::calibration::InterpolationMode::Horizontal);
        break;
    case MenuCommand::InterpolateVertical:
        interpolate_value(fastecu::calibration::InterpolationMode::Vertical);
        break;
    case MenuCommand::InterpolateBidirectional:
        interpolate_value(fastecu::calibration::InterpolationMode::Bidirectional);
        break;
    case MenuCommand::ToggleRealtime:
        toggle_realtime();
        break;
    case MenuCommand::LogToFile:
        toggle_log_to_file();
        break;
    case MenuCommand::ConnectToEcu:
        connect_to_ecu();
        break;
    case MenuCommand::DisconnectFromEcu:
        disconnect_from_ecu();
        break;
    case MenuCommand::ReadRomFromEcu:
        start_ecu_operations("read");
        break;
    case MenuCommand::TestWriteRomToEcu:
        start_ecu_operations("test_write");
        break;
    case MenuCommand::WriteRomToEcu:
        start_ecu_operations("write");
        break;
    case MenuCommand::SetLogViews:
        change_gauge_values();
        break;
    case MenuCommand::DtcWindow:
        show_dtc_window();
        break;
    case MenuCommand::HexEditor:
        show_hex_editor();
        break;
    case MenuCommand::BiuCommunication:
        show_subaru_biu_window();
        break;
    case MenuCommand::GetKey:
        show_subaru_get_key_window();
        break;
    case MenuCommand::Terminal:
        show_terminal_window();
        break;
    case MenuCommand::About:
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
        break;
    case MenuCommand::Unknown:
        qWarning() << "Unhandled menu action:" << action;
        break;
    }
}

void MainWindow::inc_dec_value(fastecu::calibration::IncrementStep step)
{
    QMdiSubWindow *w = ui->mdiArea->activeSubWindow();
    const auto id = fastecu::ui::parse_map_window_id(w);
    if (!id)
    {
        return;
    }

    QTableWidget *mapTableWidget = w->findChild<QTableWidget *>(w->objectName());
    if (!mapTableWidget)
    {
        return;
    }

    emit LOG_D("Map " + ecuCalDef[id->rom_number]->NameList.at(id->map_number) + " scaling " +
                   ecuCalDef[id->rom_number]->MapScalingNameList.at(id->map_number) +
                   " min / max: " + ecuCalDef[id->rom_number]->MinValueList.at(id->map_number) + " / " +
                   ecuCalDef[id->rom_number]->MaxValueList.at(id->map_number),
               true, true);

    auto edit = fastecu::ui::resolve_active_map_edit(w, *ecuCalDef[id->rom_number], id->map_number);
    if (!edit)
    {
        return;
    }

    const auto patch = fastecu::calibration::apply_increment(bytes::view(ecuCalDef[id->rom_number]->FullRomData),
                                                             edit->spec(), edit->x_size(), edit->cell_text(),
                                                             edit->range(), step, fileActions->float_precision);
    if (!patch.has_value())
    {
        QMessageBox::warning(this, tr("Set value"), QString::fromStdString(patch.error().detail));
        return;
    }
    fastecu::ui::apply_patch(*ecuCalDef[id->rom_number], id->map_number, edit->kind(), *patch);
    set_maptablewidget_items();
}

void MainWindow::set_value()
{
    bool bStatus;

    QMdiSubWindow *w = ui->mdiArea->activeSubWindow();
    const auto id = fastecu::ui::parse_map_window_id(w);
    if (!id)
    {
        return;
    }

    QTableWidget *mapTableWidget = w->findChild<QTableWidget *>(w->objectName());
    if (!mapTableWidget)
    {
        return;
    }

    QString text =
        QInputDialog::getText(this, tr("QInputDialog::getText()"), tr("Set value: (ie: x20 | +20 | -20 | /20 | 20)"),
                              QLineEdit::Normal, "", &bStatus);

    text.replace(",", ".");

    if (!bStatus || text.isEmpty())
    {
        return;
    }

    auto edit = fastecu::ui::resolve_active_map_edit(w, *ecuCalDef[id->rom_number], id->map_number);
    if (!edit)
    {
        return;
    }

    const auto patch = fastecu::calibration::apply_set_expression(
        bytes::view(ecuCalDef[id->rom_number]->FullRomData), edit->spec(), edit->x_size(), edit->cell_text(),
        edit->range(), text.toStdString(), fileActions->float_precision);
    if (!patch.has_value())
    {
        QMessageBox::warning(this, tr("Set value"), QString::fromStdString(patch.error().detail));
        return;
    }
    fastecu::ui::apply_patch(*ecuCalDef[id->rom_number], id->map_number, edit->kind(), *patch);
    set_maptablewidget_items();
}

void MainWindow::interpolate_value(fastecu::calibration::InterpolationMode mode)
{
    QMdiSubWindow *w = ui->mdiArea->activeSubWindow();
    const auto id = fastecu::ui::parse_map_window_id(w);
    if (!id)
    {
        return;
    }

    QTableWidget *mapTableWidget = w->findChild<QTableWidget *>(w->objectName());
    if (!mapTableWidget)
    {
        return;
    }

    auto edit = fastecu::ui::resolve_active_map_edit(w, *ecuCalDef[id->rom_number], id->map_number);
    if (!edit)
    {
        return;
    }

    const auto patch = fastecu::calibration::apply_interpolation(bytes::view(ecuCalDef[id->rom_number]->FullRomData),
                                                                 edit->spec(), edit->x_size(), edit->cell_text(),
                                                                 edit->range(), mode, fileActions->float_precision);
    if (!patch.has_value())
    {
        QMessageBox::warning(this, tr("Set value"), QString::fromStdString(patch.error().detail));
        return;
    }
    fastecu::ui::apply_patch(*ecuCalDef[id->rom_number], id->map_number, edit->kind(), *patch);
    set_maptablewidget_items();
}

void MainWindow::copy_value()
{
    QMdiSubWindow *w = ui->mdiArea->activeSubWindow();
    if (w)
    {
        QStringList mapWindowString = w->objectName().split(",");

        QTableWidget *mapTableWidget = w->findChild<QTableWidget *>(w->objectName());
        if (mapTableWidget)
        {
            QModelIndexList cells = mapTableWidget->selectionModel()->selectedIndexes();
            // qSort(cells); // Necessary, otherwise they are in column order

            QString text;
            int currentRow = 0;
            foreach (const QModelIndex& cell, cells)
            {
                if (text.length() == 0)
                {
                }
                else if (cell.row() != currentRow)
                {
                    text += '\n';
                }
                else
                {
                    text += '\t';
                }
                currentRow = cell.row();
                text += cell.data().toString();
            }

            QApplication::clipboard()->setText(text);
        }
    }
}

// Behavior change beyond routing through resolve_active_map_edit / apply_paste
// / apply_patch: pasting onto a selected axis now edits that axis (legacy
// paste_value had no axis resolution and always wrote into the map body).
// A second, incidental change rides along with that routing for a `y_size ==
// 1` map specifically -- resolve_edit_target's MapBody branch applies its
// column-offset adjustment CONDITIONALLY (only when y_size == 1), where
// legacy paste_value applied its own `-1` column offset UNCONDITIONALLY.
// For an ordinary 2D map these are identical; for a `y_size == 1` map,
// legacy produced firstCol == -1 (an out-of-bounds column, the layout bug
// map_edit_adapter.cpp's apply_patch guard now protects the write side of),
// where routing paste through resolve_active_map_edit produces the correct
// 0-based column instead.
void MainWindow::paste_value()
{
    QMdiSubWindow *w = ui->mdiArea->activeSubWindow();
    const auto id = fastecu::ui::parse_map_window_id(w);
    if (!id)
    {
        return;
    }

    QTableWidget *mapTableWidget = w->findChild<QTableWidget *>(w->objectName());
    if (!mapTableWidget)
    {
        return;
    }

    auto edit = fastecu::ui::resolve_active_map_edit(w, *ecuCalDef[id->rom_number], id->map_number);
    if (!edit)
    {
        return;
    }

    const QString pasteString = QApplication::clipboard()->text();
    const QStringList rows = pasteString.split('\n');

    std::vector<std::vector<std::string>> owned_rows;
    owned_rows.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows)
    {
        const QStringList columns = row.split('\t');
        std::vector<std::string> owned_columns;
        owned_columns.reserve(static_cast<std::size_t>(columns.size()));
        for (const auto& column : columns)
        {
            owned_columns.push_back(column.toStdString());
        }
        owned_rows.push_back(std::move(owned_columns));
    }

    std::vector<std::vector<std::string_view>> pasted_rows;
    pasted_rows.reserve(owned_rows.size());
    for (const auto& row : owned_rows)
    {
        pasted_rows.emplace_back(row.begin(), row.end());
    }

    const auto spec = edit->spec();
    const std::uint32_t x_size = edit->x_size();
    const std::uint32_t y_size = edit->kind() == fastecu::calibration::EditTargetKind::XAxis ? 1U : spec.y_size;

    const auto patch =
        fastecu::calibration::apply_paste(bytes::view(ecuCalDef[id->rom_number]->FullRomData), spec, x_size, y_size,
                                          edit->cell_text(), edit->range(), pasted_rows, fileActions->float_precision);
    if (!patch.has_value())
    {
        QMessageBox::warning(this, tr("Set value"), QString::fromStdString(patch.error().detail));
        return;
    }
    fastecu::ui::apply_patch(*ecuCalDef[id->rom_number], id->map_number, edit->kind(), *patch);
    set_maptablewidget_items();
}

int MainWindow::connect_to_ecu()
{
    ecuid.clear();
    ecu_init_complete = false;
    set_status_bar_label(false, false, "");
    serial->reset_connection();

    qDebug() << "Opening interface, please wait...";
    open_serial_port();
    if (serial->is_serial_port_open())
    {
        serial_port_list->setDisabled(true);
        refresh_serial_port_list->setDisabled(true);
        qDebug() << "Initialising ECU, please wait...";
        int loopcount = 0;
        while (!ecu_init_complete && loopcount < 5)
        {
            ecu_init();
            delay(500);
            loopcount++;
        }
        if (!ecu_init_complete)
        {
            disconnect_from_ecu();
        }
    }
    else
    {
        QMessageBox::warning(this, tr("Serial port"), "Could not open interface!");
        return STATUS_ERROR;
    }
    return STATUS_SUCCESS;
}

void MainWindow::disconnect_from_ecu()
{
    qDebug() << "Disconnecting...";
    ecuid.clear();
    ecu_init_complete = false;
    set_status_bar_label(false, false, "");
    serial->reset_connection();

    serial->set_serial_port_baudrate("4800");
    serial->set_serial_port_parity(QSerialPort::NoParity);

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
    for (int i = 0; i < configValues->romraider_definition_files.length(); i++)
    {
        new QListWidgetItem(configValues->romraider_definition_files.at(i), definition_files);
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
    QAction *logger;
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
                if (action->text() == "Logging")
                {
                    action->setChecked(state);
                }
            }
        }
    }
}

void MainWindow::toggle_realtime()
{
    using namespace std::chrono_literals;

    QAction *logger{};
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
                if (action->text() == "Logging")
                {
                    logger = action;
                    logging_state = logger->isChecked();
                }
            }
        }
    }

    if (logging_state)
    {
        qDebug() << "Start datalog";
        if (!ecu_init_complete)
        {
            if (connect_to_ecu())
            {
                restoreLoggingUiState();
                QMessageBox::information(this, tr("ECU connection"), "Unable to connect to ECU");
                return;
            }
        }
        logging_state = true;

        fastecu::desktop::logging::LogSessionConfig config;
        fastecu::logging::LoggingProtocolId protocol_id;
        fastecu::logging::LoggingPolicy logging_policy{};
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA")
        {
            config.protocolId = "MUT_DMA";
            activeLogValueProtocolFilter = "MUT_DMA";
            protocol_id = fastecu::logging::LoggingProtocolId::MutDma;
            logging_policy = {.poll_timeout = 50ms,
                              .car_silence_miss_threshold = 20,
                              .reconnect_attempt_threshold = 100,
                              .reconnect_retry_period = 20};
        }
        else if (configValues->flash_protocol_selected_log_protocol == "CDBG")
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

        auto snapshot = fastecu::desktop::logging::make_desktop_logging_snapshot(
            *logValues, protocol_id, activeLogValueProtocolFilter, logging_policy);
        if (!snapshot)
        {
            emit LOG_E("Logging session failed to start: " + QString::fromStdString(snapshot.error().detail), true,
                       true);
            restoreLoggingUiState();
            QMessageBox::information(this, tr("Logging"), "Unable to start logging");
            return;
        }

        snapshot->target_is_ecu = ecu_radio_button->isChecked();
        activeLoggingSnapshot.emplace(*snapshot);
        const auto started = loggingEngine->start(config, std::move(*snapshot));
        if (!started)
        {
            restoreLoggingUiState();
            QMessageBox::information(this, tr("Logging"), "Unable to start logging");
            return;
        }
    }
    else
    {
        qDebug() << "Stop datalog";
        if (datalog_file_open)
        {
            datalog_file_open = false;
            datalog_file.close();
        }

        loggingEngine->stop();

        // disconnect_from_ecu();
    }
}

void MainWindow::toggle_log_to_file()
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
                if (action->text() == "Log to file")
                {
                    write_datalog_to_file = action->isChecked();
                }
            }
        }
    }

    if (!write_datalog_to_file)
    {
        if (datalog_file_open)
        {
            datalog_file_open = false;
            datalog_file.close();
        }
    }
}

void MainWindow::show_dtc_window()
{
    serial->reset_connection();
    ecuid.clear();
    ecu_init_complete = false;

    QStringList spl;
    spl.append(serial_ports.at(serial_port_list->currentIndex()));
    serial->set_serial_port_list(spl);

    emit LOG_D("Starting DTC operations", true, true);

    fastecu::diagnostics::SerialDiagnosticLink link(serial);
    DtcOperations dtcOperations(link, this);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_E, syslogger, &SystemLogger::log_messages);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_W, syslogger, &SystemLogger::log_messages);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_I, syslogger, &SystemLogger::log_messages);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_D, syslogger, &SystemLogger::log_messages);

    dtcOperations.exec();

    emit LOG_D("DTC operations stopped", true, true);
}

void MainWindow::show_hex_editor()
{
    emit LOG_D("Show hex editor", true, true);

    int rom_number = 0;

    QTreeWidgetItem *selectedItem = nullptr;
    int item_count = ui->calibrationFilesTreeWidget->selectedItems().count();
    if (item_count)
    {
        selectedItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);
        rom_number = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selectedItem);

        // The window shows itself from its own constructor and is parented to
        // MainWindow, so without WA_DeleteOnClose every invocation left a
        // closed-but-alive HexEdit -- and its copy of the ROM data -- alive
        // until MainWindow was destroyed. HexEdit::closeEvent() ignores the
        // event when the user cancels a save prompt, and Qt only deletes on an
        // accepted close, so cancelling still keeps the window.
        HexEdit *hexEdit = new HexEdit(ecuCalDef[rom_number], this);
        hexEdit->setAttribute(Qt::WA_DeleteOnClose);
    }
}

void MainWindow::show_preferences_window()
{
    Settings settings(*fileActions, configValues);
    settings.exec();
    // fileActions->save_config_file();
}

void MainWindow::show_subaru_biu_window()
{
    serial->reset_connection();
    ecuid.clear();
    ecu_init_complete = false;
    serial->set_add_iso14230_header(false);
    serial->set_is_iso14230_connection(true);
    open_serial_port();
    serial->change_port_speed("10400");
    // serial->change_port_speed("4800");

    fastecu::diagnostics::SerialDiagnosticLink link(serial);
    BiuOperationsSubaru biuOperationsSubaru(link, this);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_E, syslogger, &SystemLogger::log_messages);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_W, syslogger, &SystemLogger::log_messages);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_I, syslogger, &SystemLogger::log_messages);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_D, syslogger, &SystemLogger::log_messages);

    biuOperationsSubaru.exec();

    emit LOG_D("BIU stopped", true, true);

    serial->set_add_iso14230_header(false);
}

void MainWindow::show_terminal_window()
{
    QStringList serial_port_arg;
    serial_port_arg.append(serial_ports.at(serial_port_list->currentIndex()));
    serial->set_serial_port_list(serial_port_arg);
    fastecu::diagnostics::SerialDiagnosticLink link(serial);
    DataTerminal hexCommander(link, this);
    QObject::connect(&hexCommander, &DataTerminal::LOG_E, syslogger, &SystemLogger::log_messages);
    QObject::connect(&hexCommander, &DataTerminal::LOG_W, syslogger, &SystemLogger::log_messages);
    QObject::connect(&hexCommander, &DataTerminal::LOG_I, syslogger, &SystemLogger::log_messages);
    QObject::connect(&hexCommander, &DataTerminal::LOG_D, syslogger, &SystemLogger::log_messages);

    hexCommander.exec();
}

void MainWindow::show_subaru_get_key_window()
{

    GetKeyOperationsSubaru getKeyOperationsSubaru(this);
    getKeyOperationsSubaru.exec();
}

void MainWindow::winols_csv_to_romraider_xml()
{
    DefinitionFileConvert definitionFileMaker;
    definitionFileMaker.exec();
}

void MainWindow::set_maptablewidget_items()
{
    int mapRomNumber = 0;
    int mapNumber = 0;
    QString mapName = "";

    QMdiSubWindow *w = ui->mdiArea->activeSubWindow();
    if (w)
    {
        QStringList mapWindowString = w->objectName().split(",");
        mapRomNumber = mapWindowString.at(0).toInt();
        mapNumber = mapWindowString.at(1).toInt();
        mapName = mapWindowString.at(2);

        QTableWidget *mapTableWidget = w->findChild<QTableWidget *>(w->objectName());
        if (mapTableWidget)
        {
            int xSize = ecuCalDef[mapRomNumber]->XSizeList.at(mapNumber).toInt();
            int ySize = ecuCalDef[mapRomNumber]->YSizeList.at(mapNumber).toInt();
            int mapSize = xSize * ySize;

            int xSizeOffset = 0;
            int ySizeOffset = 0;

            if (ecuCalDef[mapRomNumber]->YSizeList.at(mapNumber).toInt() > 1)
            {
                xSizeOffset = 1;
            }
            if (ecuCalDef[mapRomNumber]->XSizeList.at(mapNumber).toInt() > 1 ||
                ecuCalDef[mapRomNumber]->XScaleTypeList.at(mapNumber) == "Static Y Axis" ||
                ecuCalDef[mapRomNumber]->XScaleTypeList.at(mapNumber) == "Static X Axis")
            {
                ySizeOffset = 1;
            }

            QFont cellFont = mapTableWidget->font();
            cellFont.setPointSize(cellFontSize);
            cellFont.setFamily("Franklin Gothic");

            if (xSize > 1)
            {
                QStringList xScaleCellText = ecuCalDef[mapRomNumber]->XScaleData.at(mapNumber).split(",");
                for (int i = 0; i < xSize; i++)
                {
                    QTableWidgetItem *cellItem;
                    if (ySize > 1)
                    {
                        cellItem = mapTableWidget->item(0, i + 1);
                    }
                    else
                    {
                        cellItem = mapTableWidget->item(0, i);
                    }

                    cellItem->setTextAlignment(Qt::AlignCenter);
                    cellItem->setFont(cellFont);

                    QString xScaleCellDataText;

                    if (xScaleCellText.at(i) == " ")
                    {
                        xScaleCellText.insert(i, QString::number(i));
                        xScaleCellDataText = xScaleCellText.at(i);
                    }
                    else if (ecuCalDef[mapRomNumber]->XScaleTypeList.at(mapNumber) == "Static Y Axis" ||
                             ecuCalDef[mapRomNumber]->XScaleTypeList.at(mapNumber) == "Static X Axis")
                    {
                        xScaleCellDataText = xScaleCellText.at(i);
                    }
                    else
                    {
                        xScaleCellDataText =
                            QString::number(xScaleCellText.at(i).toFloat(), 'f',
                                            fastecu::calibration::map_value_decimal_count(
                                                ecuCalDef[mapRomNumber]->XScaleFormatList.at(mapNumber).toStdString()));
                    }

                    if (i < xScaleCellText.count())
                    {
                        cellItem->setText(xScaleCellDataText);
                    }
                }
            }
            if (ySize > 1)
            {
                QStringList yScaleCellText = ecuCalDef[mapRomNumber]->YScaleData.at(mapNumber).split(",");
                for (int i = 0; i < ySize; i++)
                {
                    QTableWidgetItem *cellItem;
                    cellItem = mapTableWidget->item(i + 1, 0);

                    cellItem->setTextAlignment(Qt::AlignCenter);
                    cellItem->setFont(cellFont);
                    if (i < yScaleCellText.count())
                    {
                        cellItem->setText(QString::number(
                            yScaleCellText.at(i).toFloat(), 'f',
                            fastecu::calibration::map_value_decimal_count(
                                ecuCalDef[mapRomNumber]->YScaleFormatList.at(mapNumber).toStdString())));
                    }
                }
            }
            QStringList mapDataCellText = ecuCalDef[mapRomNumber]->MapData.at(mapNumber).split(",");
            for (int i = 0; i < mapSize; i++)
            {
                int yPos = 0;
                int xPos = 0;
                if (ecuCalDef[mapRomNumber]->XSizeList.at(mapNumber).toUInt() > 1)
                {
                    yPos = i / xSize + ySizeOffset;
                }
                else
                {
                    yPos = i / xSize;
                }
                if (ecuCalDef[mapRomNumber]->YSizeList.at(mapNumber).toUInt() > 1)
                {
                    xPos = i - (yPos - ySizeOffset) * xSize + xSizeOffset;
                }
                else
                {
                    xPos = i - (yPos - ySizeOffset) * xSize;
                }

                // qDebug() << "X pos:" << xPos << "Y pos:" << yPos;
                QTableWidgetItem *cellItem; // = new QTableWidgetItem;
                cellItem = mapTableWidget->item(yPos, xPos);

                cellItem->setTextAlignment(Qt::AlignCenter);
                cellItem->setFont(cellFont);
                cellItem->setBackground(
                    QBrush(get_map_cell_color(ecuCalDef[mapRomNumber], mapDataCellText.at(i).toFloat(), mapNumber)));
                // if (ecuCalDef[mapRomNumber]->TypeList.at(mapNumber) == "1D")
                cellItem->setForeground(Qt::black);
                // else
                //     cellItem->setForeground(Qt::white);

                if (i < mapDataCellText.count())
                {
                    cellItem->setText(
                        QString::number(mapDataCellText.at(i).toFloat(), 'f',
                                        fastecu::calibration::map_value_decimal_count(
                                            ecuCalDef[mapRomNumber]->FormatList.at(mapNumber).toStdString())));
                }
            }
        }
    }
}

QColor MainWindow::get_map_cell_color(FileActions::EcuCalDefStructure *ecuCalDef, float mapDataValue, int mapIndex)
{
    const float mapMinValue = ecuCalDef->MapCellColorMin.at(mapIndex).toFloat();
    const float mapMaxValue = ecuCalDef->MapCellColorMax.at(mapIndex).toFloat();

    // Maps mapMinValue -> hue 0, mapMaxValue -> hue 210/360, clamping to that
    // range at both ends. mapMinValue == mapMaxValue would divide by zero,
    // producing a non-finite hue that's undefined (effectively invalid) as a
    // QColor::fromHsvF argument -- guarded to 0.0 instead, a well-defined
    // choice consistent with the clamp.
    constexpr double kScaleStart = 210.0 / 360.0;
    double color_value = 0.0;
    if (mapMaxValue != mapMinValue)
    {
        color_value =
            std::clamp(kScaleStart * (mapDataValue - mapMinValue) / (mapMaxValue - mapMinValue), 0.0, kScaleStart);
    }

    return QColor::fromHsvF(color_value, 0.85, 0.85);
}
