#include "src/ui/desktop/widgets/calibration_maps.h"
#include <ui_calibration_map_table.h>
#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <algorithm>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QSignalBlocker>
#include <QTableWidgetSelectionRange>
#include <QScopeGuard>
#include <QLabel>

CalibrationMaps::CalibrationMaps(fastecu::calibration::CalibrationWorkspace& workspace,
                                 fastecu::calibration::SessionId session, int mapIndex, QRect mdiAreaSize,
                                 QWidget *parent)
    : QWidget(parent), workspace_(workspace), session_(session), map_index_(mapIndex), mdi_area_size_(mdiAreaSize),
      ui{std::make_unique<Ui::CalibrationMaps>()}
{
    ui->setupUi(this);
    ui->mapDataTableWidget->installEventFilter(this);
    this->setObjectName(fastecu::ui::session_key_text(session_) + "," + QString::number(map_index_) + ",,");
    this->setAttribute(Qt::WA_DeleteOnClose);
    ui->mapNameLabel->clear();
    ui->xScaleUnitsLabel->clear();
    ui->mapDataUnitsLabel->clear();
    map_error_label_ = new QLabel(this);
    map_error_label_->setObjectName("mapDecodeError");
    map_error_label_->setTextFormat(Qt::PlainText);
    map_error_label_->setWordWrap(true);
    map_error_label_->hide();
    ui->verticalLayout->addWidget(map_error_label_);
    const auto *rom = workspace_.find(session_);
    if (rom != nullptr && rom->definition() != nullptr && map_index_ >= 0 &&
        static_cast<std::size_t>(map_index_) < rom->definition()->definition.maps.size())
    {
        const auto& name = rom->definition()->definition.maps[static_cast<std::size_t>(map_index_)].name;
        setObjectName(fastecu::ui::session_key_text(session_) + "," + QString::number(map_index_) + "," +
                      QString::fromStdString(name));
        setWindowTitle(QString::fromStdString(name) + " - " + QString::fromStdString(rom->source().display_name));
        ui->mapNameLabel->setText(QString::fromStdString(name));
    }
    refresh();
}

void CalibrationMaps::initialize_view(const fastecu::ui::MapPresentation& map,
                                      const fastecu::calibration::RomSource& source)
{
    QString mapWindowObjectName =
        fastecu::ui::session_key_text(session_) + "," + QString::number(map_index_) + "," + map.name;

    this->setObjectName(mapWindowObjectName);
    this->setWindowTitle(map.name + " - " + QString::fromStdString(source.display_name));

    QString xScaleUnitsTitle = "";
    if (map.x_name != " ")
    {
        if (map.x_units != " ")
        {
            xScaleUnitsTitle = map.x_name + " (" + map.x_units + ")";
        }
        else
        {
            xScaleUnitsTitle = map.x_name;
        }
    }
    ui->xScaleUnitsLabel->setText(xScaleUnitsTitle);

    if (!map.units.isEmpty())
    {
        ui->mapDataUnitsLabel->setText(map.units);
    }

    numeric_body_ = map.type != "Switch" && map.type != "MultiSelectable" && map.type != "Selectable";
    if (map.type == "Switch")
    {
        // qDebug() << "Switchable map";
        mapWindowObjectName = mapWindowObjectName + "," + "Switch";
        map_cell_width = map_cell_width_selectable;
        x_size = 1;
        y_size = 1;
        ui->xScaleUnitsLabel->setFixedHeight(0);
    }
    if (map.type == "MultiSelectable")
    {
        // qDebug() << "MultiSelectable map";
        mapWindowObjectName = mapWindowObjectName + "," + "MultiSelectable";
        map_cell_width = map_cell_width_selectable;
        x_size = 1;
        y_size = 1;
        ui->xScaleUnitsLabel->setFixedHeight(0);
    }
    if (map.type == "Selectable")
    {
        // qDebug() << "Selectable map";
        mapWindowObjectName = mapWindowObjectName + "," + "Selectable";
        map_cell_width = map_cell_width_selectable;
        x_size = 1;
        y_size = 1;
        ui->xScaleUnitsLabel->setFixedHeight(0);
    }
    if (map.type == "1D")
    {
        qDebug() << "1D map";
        mapWindowObjectName = mapWindowObjectName + "," + "1D";
        map_cell_width = map_cell_width1_d;
        x_size = 1;
        y_size = 1;
        ui->xScaleUnitsLabel->setFixedHeight(0);
    }
    if (map.type == "2D")
    {
        // qDebug() << "2D map" << map.name << map.x_size <<
        // map.y_size;
        if (map.y_size > 1 || map.x_size > 1)
        {
            this->setWindowIcon(QIcon(":/icons/2D-64-W.png"));
        }
        else
        {
            this->setWindowIcon(QIcon(":/icons/1D-64-W.png"));
        }
        if (map.x_type == "Static Y Axis")
        {
            mapWindowObjectName = mapWindowObjectName + "," + "Static Y Axis";
        }
        if (map.x_type == "Static X Axis")
        {
            mapWindowObjectName = mapWindowObjectName + "," + "Static X Axis";
        }
        else if (map.y_size > 1)
        {
            mapWindowObjectName = mapWindowObjectName + "," + "Y Axis";
        }
        else
        {
            mapWindowObjectName = mapWindowObjectName + "," + "X Axis";
        }
        x_size_offset = 0;
        y_size_offset = 0;
        if (map.y_size > 1)
        {
            x_size_offset = 1;
        }
        if (map.x_size > 1 || map.x_type == "Static Y Axis" || map.x_type == "Static X Axis")
        {
            y_size_offset = 1;
        }
        x_size = map.x_size + x_size_offset;
        y_size = map.y_size + y_size_offset;
    }
    if (map.type == "3D")
    {
        // qDebug() << "3D map" << map.name << map.x_size <<
        // map.y_size;
        this->setWindowIcon(QIcon(":/icons/3D-64-W.png"));
        mapWindowObjectName = mapWindowObjectName + "," + "3D";
        x_size_offset = 0;
        y_size_offset = 0;
        if (map.y_size > 1)
        {
            x_size_offset = 1;
        }
        if (map.x_size > 1)
        {
            y_size_offset = 1;
        }
        x_size = map.x_size + x_size_offset;
        y_size = map.y_size + y_size_offset;

        auto *yScaleUnitsLabel = findChild<QLabel *>("mapYAxisUnits");
        if (yScaleUnitsLabel == nullptr)
        {
            yScaleUnitsLabel = new VerticalLabel();
            yScaleUnitsLabel->setObjectName("mapYAxisUnits");
            ui->horizontalLayout_2->insertWidget(0, yScaleUnitsLabel);
        }
        yScaleUnitsLabel->setAlignment(Qt::AlignCenter);
        yScaleUnitsLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
        QString yScaleUnitsTitle = map.y_name + " (" + map.y_units + ")";
        yScaleUnitsLabel->setText(yScaleUnitsTitle);
    }

    // qDebug() << "Create map" << mapWindowObjectName;
    this->setObjectName(mapWindowObjectName);
    ui->mapDataTableWidget->setObjectName(mapWindowObjectName);
    ui->mapDataTableWidget->setColumnCount(x_size);
    ui->mapDataTableWidget->setRowCount(y_size);
    ui->mapDataTableWidget->setStyleSheet("QTableWidget::item { padding: 3px }");
    ui->mapNameLabel->setText(map.name);

    if (map.type == "3D")
    {
        QTableWidgetItem *cellItem = new QTableWidgetItem;
        cellItem->setFlags(Qt::NoItemFlags);
        cellItem->setBackground(QBrush(QColor(0xf0, 0xf0, 0xf0, 255)));
        ui->mapDataTableWidget->setItem(0, 0, cellItem);
    }

    if (map.type != "Selectable" && map.type != "Switch")
    {
        connect(ui->mapDataTableWidget, &QTableWidget::cellClicked, this, &CalibrationMaps::cellClicked,
                Qt::UniqueConnection);
        connect(ui->mapDataTableWidget, &QTableWidget::cellPressed, this, &CalibrationMaps::cellPressed,
                Qt::UniqueConnection);
        /// connect(ui->mapDataTableWidget, SIGNAL(cellEntered(int, int)), this, SLOT (cellActivated(int, int)));
        connect(ui->mapDataTableWidget, &QTableWidget::currentCellChanged, this, &CalibrationMaps::cellChanged,
                Qt::UniqueConnection);
    }
    view_initialized_ = true;
}

void CalibrationMaps::show_map_error(const fastecu::Error& error)
{
    const QSignalBlocker blocker(ui->mapDataTableWidget);
    ui->mapDataTableWidget->clear();
    ui->mapDataTableWidget->setRowCount(0);
    ui->mapDataTableWidget->setColumnCount(0);
    ui->mapDataTableWidget->setEnabled(false);
    map_error_label_->setText(QString::fromStdString(error.detail));
    map_error_label_->show();
    view_initialized_ = false;
}

bool CalibrationMaps::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->mapDataTableWidget && event->type() == QEvent::KeyPress && numeric_body_ &&
        static_cast<QKeyEvent *>(event)->matches(QKeySequence::SelectAll))
    {
        const auto *rom = workspace_.find(session_);
        const auto range = rom == nullptr
                               ? std::nullopt
                               : fastecu::ui::body_widget_range(*rom, map_index_, ui->mapDataTableWidget->rowCount(),
                                                                ui->mapDataTableWidget->columnCount());
        if (range.has_value())
        {
            ui->mapDataTableWidget->clearSelection();
            ui->mapDataTableWidget->setRangeSelected(
                QTableWidgetSelectionRange(range->first_row, range->first_col, range->last_row, range->last_col), true);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

CalibrationMaps::~CalibrationMaps()
{
}

void CalibrationMaps::setMapTableWidgetSize(int maxWidth, int maxHeight, int xSize_arg)
{
    int w = 0;
    int h = 0;

    w += ui->mapDataTableWidget->contentsMargins().left() + ui->mapDataTableWidget->contentsMargins().right();
    h += ui->mapDataTableWidget->contentsMargins().top() + ui->mapDataTableWidget->contentsMargins().bottom();
    w += ui->mapDataTableWidget->verticalHeader()->width();
    for (int i = 0; i < ui->mapDataTableWidget->columnCount(); i++)
    {
        w += ui->mapDataTableWidget->columnWidth(i);
    }
    for (int i = 0; i < ui->mapDataTableWidget->rowCount(); i++)
    {
        h += ui->mapDataTableWidget->rowHeight(i);
    }

    if (w > maxWidth)
    {
        w = maxWidth - 40;
        h += ui->mapDataTableWidget->horizontalHeader()->height() + 15;
    }
    else
    {
        h += ui->mapDataTableWidget->horizontalHeader()->height();
    }
    if (h > maxHeight)
    {
        h = maxHeight - 40;
        w += ui->mapDataTableWidget->verticalHeader()->width() + 15;
    }
    else
    {
        w += ui->mapDataTableWidget->verticalHeader()->width();
    }

    ui->mapDataTableWidget->setMinimumWidth(w);
    ui->mapDataTableWidget->setMaximumWidth(w);
    ui->mapDataTableWidget->setMinimumHeight(h);
    ui->mapDataTableWidget->setMaximumHeight(h);

    ui->mapDataTableWidget->setFixedWidth(w);
    ui->mapDataTableWidget->setFixedHeight(h);
}

void CalibrationMaps::refresh()
{
    const auto *rom = workspace_.find(session_);
    if (rom == nullptr)
    {
        return;
    }
    const auto shown = fastecu::ui::present_map(*rom, static_cast<std::size_t>(map_index_));
    if (!shown.has_value())
    {
        show_map_error(shown.error());
        return;
    }
    const auto& map = *shown;
    map_error_label_->hide();
    ui->mapDataTableWidget->setEnabled(true);
    const bool initialized = !view_initialized_;
    if (initialized)
    {
        initialize_view(map, rom->source());
    }
    if (!color_bounds_.has_value())
    {
        color_bounds_ = fastecu::ui::opening_color_bounds(map);
    }
    const auto resize = qScopeGuard(
        [this, initialized]
        {
            if (initialized)
            {
                ui->mapDataTableWidget->horizontalHeader()->resizeSections(QHeaderView::ResizeToContents);
                ui->mapDataTableWidget->verticalHeader()->resizeSections(QHeaderView::ResizeToContents);
                setMapTableWidgetSize(mdi_area_size_.width() - 15, mdi_area_size_.height() - 15, x_size);
            }
        });
    const QSignalBlocker table_blocker(ui->mapDataTableWidget);
    QFont font = ui->mapDataTableWidget->font();
    font.setPointSize(cell_font_size);
    font.setFamily("Franklin Gothic");

    if (map.type == "Switch")
    {
        // The retained typed formats resolve RomRaider switches to Selectable.
        // No format populates legacy StateList; preserve the remaining empty
        // switch control without introducing a new definition representation.
        auto *checkbox = qobject_cast<QCheckBox *>(ui->mapDataTableWidget->cellWidget(0, 0));
        if (checkbox == nullptr)
        {
            checkbox = new QCheckBox("On/Off");
            ui->mapDataTableWidget->setCellWidget(0, 0, checkbox);
            connect(checkbox, &QCheckBox::checkStateChanged, this,
                    [this](Qt::CheckState state) { emit checkbox_state_changed(static_cast<int>(state)); });
        }
        const QSignalBlocker blocker(checkbox);
        checkbox->setChecked(false);
        return;
    }
    if (map.type == "Selectable" || map.type == "MultiSelectable")
    {
        const bool multi = map.type == "MultiSelectable";
        const auto labels = map.y_units.split(',');
        for (int row = 0; row < (multi ? labels.size() : 1); ++row)
        {
            if (multi)
            {
                auto *item = new QTableWidgetItem(labels[row]);
                item->setTextAlignment(Qt::AlignCenter);
                item->setFont(font);
                ui->mapDataTableWidget->setItem(row, 0, item);
            }
            const int col = multi ? 1 : 0;
            auto *combo = qobject_cast<QComboBox *>(ui->mapDataTableWidget->cellWidget(row, col));
            if (combo == nullptr)
            {
                combo = new QComboBox;
                combo->setFont(font);
                combo->setFixedWidth(map_cell_width_selectable);
                combo->setObjectName("selectableComboBox");
                const QSignalBlocker blocker(combo);
                for (int i = 0; i < map.selection_names.size(); ++i)
                {
                    if (multi || !map.selection_names[i].isEmpty())
                    {
                        combo->addItem(map.selection_names[i]);
                    }
                }
                if (multi)
                {
                    // Preserve the explicit empty choice of the existing control.
                    combo->addItem(map.selection_names.isEmpty() ? " " : "");
                }
                ui->mapDataTableWidget->setCellWidget(row, col, combo);
                connect(combo, &QComboBox::currentTextChanged, this,
                        &CalibrationMaps::selectable_combobox_item_changed);
            }
            const QSignalBlocker blocker(combo);
            int current = multi && !map.body.empty() ? fastecu::ui::selection_index(map.body.front()) : 0;
            if (!multi)
            {
                for (std::size_t i = 0; i < map.selection_values.size(); ++i)
                {
                    if (map.blob.has_value() && map.selection_values[i] == *map.blob)
                    {
                        current = static_cast<int>(i);
                    }
                }
            }
            combo->setCurrentIndex(current);
        }
        if (!multi)
        {
            return;
        }
        // The existing MultiSelectable layout has one visible column. Its
        // numeric rendering follows control creation and replaces the label.
    }

    const auto cell = [&](int row, int col, const fastecu::ui::PresentedCell& value, const QColor& background)
    {
        auto *item = ui->mapDataTableWidget->item(row, col);
        if (item == nullptr)
        {
            item = new QTableWidgetItem;
            ui->mapDataTableWidget->setItem(row, col, item);
        }
        item->setTextAlignment(Qt::AlignCenter);
        item->setFont(font);
        item->setForeground(Qt::black);
        item->setBackground(background);
        item->setText(value.text);
        item->setToolTip(value.diagnostic);
    };
    const bool static_x = map.x_type == "Static Y Axis" || map.x_type == "Static X Axis";
    const auto axis_cell = [](const std::optional<std::vector<fastecu::ui::PresentedCell>>& axis, int index)
    {
        return axis.has_value() ? axis->at(static_cast<std::size_t>(index))
                                : fastecu::ui::PresentedCell{.text = QString::number(index)};
    };
    if (map.type != "1D")
    {
        if (map.y_size > 1 && (map.x_size <= 1 || !static_x))
        {
            for (int i = 0; i < map.y_size; ++i)
            {
                cell(i + y_size_offset, 0, axis_cell(map.y_axis, i), Qt::white);
            }
        }
        for (int i = 0; i < map.x_size; ++i)
        {
            const auto value = axis_cell(map.x_axis, i);
            cell(0, i + x_size_offset, value, Qt::white);
            const int width = QFontMetrics(font).horizontalAdvance(value.text) + 20;
            ui->mapDataTableWidget->horizontalHeader()->resizeSection(i + x_size_offset, width);
        }
    }
    for (int i = 0; i < map.x_size * map.y_size; ++i)
    {
        const int row = i / map.x_size + y_size_offset;
        const int col = i % map.x_size + x_size_offset;
        const auto& value = map.body.at(static_cast<std::size_t>(i));
        const auto background = map.type != "1D" && value.numeric_value.has_value() && color_bounds_.has_value()
                                    ? fastecu::ui::map_cell_color(*value.numeric_value, *color_bounds_)
                                    : QColor(Qt::white);
        cell(row, col, value, background);
    }
}

void CalibrationMaps::cellClicked(int row, int col)
{
    start_col = col;
    start_row = row;
    // qDebug() << "Cell" << col << ":" << row << "clicked";

    QStringList objectName = ui->mapDataTableWidget->objectName().split(",");
    int cols = ui->mapDataTableWidget->columnCount();
    int rows = ui->mapDataTableWidget->rowCount();

    for (int i = 0; i < cols; i++)
    {
        for (int j = 0; j < rows; j++)
        {
            if (ui->mapDataTableWidget->item(j, i) != nullptr)

            {

                ui->mapDataTableWidget->item(j, i)->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEditable |
                                                             Qt::ItemIsEnabled);
            }
        }
    }
    ui->mapDataTableWidget->item(row, col)->setSelected(true);
    if (objectName.at(3) == "3D")
    {
        ui->mapDataTableWidget->item(0, 0)->setFlags(Qt::ItemIsEditable);
    }

    if ((objectName.at(3) == "Static Y Axis" || objectName.at(3) == "Static X Axis") && rows > 1)
    {
        for (int j = 0; j < cols; j++)
        {
            ui->mapDataTableWidget->item(0, j)->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled);
        }
    }
}

void CalibrationMaps::cellPressed(int row, int col)
{
    start_col = col;
    start_row = row;
    // qDebug() << "Cell" << col << ":" << row << "pressed";

    int cols = ui->mapDataTableWidget->columnCount();
    int rows = ui->mapDataTableWidget->rowCount();

    QStringList objectName = ui->mapDataTableWidget->objectName().split(",");
    for (int i = 0; i < cols; i++)
    {
        for (int j = 0; j < rows; j++)
        {
            if (ui->mapDataTableWidget->item(j, i) != nullptr)

            {

                ui->mapDataTableWidget->item(j, i)->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEditable |
                                                             Qt::ItemIsEnabled);
            }
        }
    }
    ui->mapDataTableWidget->item(row, col)->setSelected(true);
    if (objectName.at(3) == "3D")
    {
        ui->mapDataTableWidget->item(0, 0)->setFlags(Qt::ItemIsEditable);
    }

    if ((objectName.at(3) == "Static Y Axis" || objectName.at(3) == "Static X Axis") && rows > 1)
    {
        for (int j = 0; j < cols; j++)
        {
            ui->mapDataTableWidget->item(0, j)->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled);
        }
    }
}

void CalibrationMaps::cellChanged(int curRow, int curCol, int prevRow, int prevCol)
{
    // qDebug() << "Startcell =" << startCol << ":" << startRow << " and current cell is" << curCol << ":" << curRow;

    int cols = ui->mapDataTableWidget->columnCount();
    int rows = ui->mapDataTableWidget->rowCount();

    // qDebug() << "cellChanged" << ui->mapDataTableWidget->objectName().split(",").at(3);

    /* Check for 3D table */
    QStringList objectName = ui->mapDataTableWidget->objectName().split(",");
    if (start_col == 0 && objectName.at(3) == "3D")
    {
        for (int i = 1; i < cols; i++)
        {
            for (int j = 0; j < rows; j++)
            {
                // ui->mapDataTableWidget->item(0, i)->setFlags(Qt::ItemIsEditable|Qt::ItemIsEnabled);
                if (ui->mapDataTableWidget->item(j, i) != nullptr)

                {

                    ui->mapDataTableWidget->item(j, i)->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled);
                }
            }
        }
    }
    else if (start_row == 0 &&
             (objectName.at(3) == "3D" || objectName.at(3) == "X Axis" || objectName.at(3) == "Y Axis"))
    {
        for (int i = 0; i < cols; i++)
        {
            for (int j = 1; j < rows; j++)
            {
                // ui->mapDataTableWidget->item(j, 0)->setFlags(Qt::ItemIsEditable|Qt::ItemIsEnabled);
                if (ui->mapDataTableWidget->item(j, i) != nullptr)

                {

                    ui->mapDataTableWidget->item(j, i)->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled);
                }
            }
        }
    }
    else
    {
        if (objectName.at(3) == "3D" || objectName.at(3) == "X Axis")
        {
            for (int i = 0; i < cols; i++)
            {
                ui->mapDataTableWidget->item(0, i)->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled);
                ui->mapDataTableWidget->item(0, i)->setSelected(false);
            }
        }
        if (objectName.at(3) == "3D")
        {
            for (int j = 0; j < rows; j++)
            {
                ui->mapDataTableWidget->item(j, 0)->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled);
                ui->mapDataTableWidget->item(j, 0)->setSelected(false);
            }
        }
    }
    if (objectName.at(3) == "3D")
    {
        ui->mapDataTableWidget->item(0, 0)->setFlags(Qt::ItemIsEditable);
    }

    if ((objectName.at(3) == "Static Y Axis" || objectName.at(3) == "Static X Axis") && rows > 1)
    {
        for (int j = 0; j < cols; j++)
        {
            ui->mapDataTableWidget->item(0, j)->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled);
        }
    }
}
