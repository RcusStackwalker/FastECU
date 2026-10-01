#include "src/ui/desktop/widgets/calibration_maps.h"
#include <ui_calibration_map_table.h>

#include <algorithm>
#include <QSignalBlocker>

CalibrationMaps::CalibrationMaps(fastecu::calibration::CalibrationWorkspace& workspace,
                                 fastecu::calibration::SessionId session, int mapIndex, QRect mdiAreaSize,
                                 QWidget *parent)
    : QWidget(parent), workspace_(workspace), session_(session), map_index_(mapIndex),
      ui{std::make_unique<Ui::CalibrationMaps>()}
{
    ui->setupUi(this);
    this->setObjectName(fastecu::ui::session_key_text(session_) + "," + QString::number(map_index_) + ",,");
    this->setAttribute(Qt::WA_DeleteOnClose);
    const auto *rom = workspace_.find(session_);
    if (rom == nullptr)
    {
        return;
    }
    const auto shown = fastecu::ui::present_map(*rom, static_cast<std::size_t>(map_index_));
    if (!shown.has_value())
    {
        return;
    }
    const auto& map = *shown;
    color_bounds_ = fastecu::ui::opening_color_bounds(map);

    this->setParent(parent);

    QString mapWindowObjectName =
        fastecu::ui::session_key_text(session) + "," + QString::number(mapIndex) + "," + map.name;

    this->setObjectName(mapWindowObjectName);
    this->setWindowTitle(map.name + " - " + QString::fromStdString(rom->source().display_name));

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

    if (map.type == "Switch")
    {
        // qDebug() << "Switchable map";
        mapWindowObjectName = mapWindowObjectName + "," + "Switch";
        mapCellWidth = mapCellWidthSelectable;
        xSize = 1;
        ySize = 1;
        ui->xScaleUnitsLabel->setFixedHeight(0);
    }
    if (map.type == "MultiSelectable")
    {
        // qDebug() << "MultiSelectable map";
        mapWindowObjectName = mapWindowObjectName + "," + "MultiSelectable";
        mapCellWidth = mapCellWidthSelectable;
        xSize = 1;
        ySize = 1;
        ui->xScaleUnitsLabel->setFixedHeight(0);
    }
    if (map.type == "Selectable")
    {
        // qDebug() << "Selectable map";
        mapWindowObjectName = mapWindowObjectName + "," + "Selectable";
        mapCellWidth = mapCellWidthSelectable;
        xSize = 1;
        ySize = 1;
        ui->xScaleUnitsLabel->setFixedHeight(0);
    }
    if (map.type == "1D")
    {
        qDebug() << "1D map";
        mapWindowObjectName = mapWindowObjectName + "," + "1D";
        mapCellWidth = mapCellWidth1D;
        xSize = 1;
        ySize = 1;
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
        else if (map.x_size > 1)
        {
            mapWindowObjectName = mapWindowObjectName + "," + "X Axis";
        }
        else
        {
            mapWindowObjectName = mapWindowObjectName + "," + "X Axis";
        }
        xSizeOffset = 0;
        ySizeOffset = 0;
        if (map.y_size > 1)
        {
            xSizeOffset = 1;
        }
        if (map.x_size > 1 || map.x_type == "Static Y Axis" || map.x_type == "Static X Axis")
        {
            ySizeOffset = 1;
        }
        xSize = map.x_size + xSizeOffset;
        ySize = map.y_size + ySizeOffset;
    }
    if (map.type == "3D")
    {
        // qDebug() << "3D map" << map.name << map.x_size <<
        // map.y_size;
        this->setWindowIcon(QIcon(":/icons/3D-64-W.png"));
        mapWindowObjectName = mapWindowObjectName + "," + "3D";
        xSizeOffset = 0;
        ySizeOffset = 0;
        if (map.y_size > 1)
        {
            xSizeOffset = 1;
        }
        if (map.x_size > 1)
        {
            ySizeOffset = 1;
        }
        xSize = map.x_size + xSizeOffset;
        ySize = map.y_size + ySizeOffset;

        VerticalLabel *yScaleUnitsLabel = new VerticalLabel();
        yScaleUnitsLabel->setAlignment(Qt::AlignCenter);
        yScaleUnitsLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
        ui->horizontalLayout_2->insertWidget(0, yScaleUnitsLabel);
        QString yScaleUnitsTitle = map.y_name + " (" + map.y_units + ")";
        yScaleUnitsLabel->setText(yScaleUnitsTitle);
    }

    // qDebug() << "Create map" << mapWindowObjectName;
    this->setObjectName(mapWindowObjectName);
    ui->mapDataTableWidget->setObjectName(mapWindowObjectName);
    ui->mapDataTableWidget->setColumnCount(xSize);
    ui->mapDataTableWidget->setRowCount(ySize);
    ui->mapDataTableWidget->setStyleSheet("QTableWidget::item { padding: 3px }");
    ui->mapNameLabel->setText(map.name);

    if (map.type == "3D")
    {
        QTableWidgetItem *cellItem = new QTableWidgetItem;
        cellItem->setFlags(Qt::NoItemFlags);
        cellItem->setBackground(QBrush(QColor(0xf0, 0xf0, 0xf0, 255)));
        ui->mapDataTableWidget->setItem(0, 0, cellItem);
    }

    refresh();
    ui->mapDataTableWidget->horizontalHeader()->resizeSections(QHeaderView::ResizeToContents);
    ui->mapDataTableWidget->verticalHeader()->resizeSections(QHeaderView::ResizeToContents);
    /*
        if (ui->mapDataTableWidget->horizontalHeader()->width() < mapCellWidth)
        {
            for (int col = 0; col < ui->mapDataTableWidget->columnCount(); col++)
                ui->mapDataTableWidget->horizontalHeader()->resizeSection(col, mapCellWidth);
        }

        if (ui->mapDataTableWidget->verticalHeader()->height() < mapCellHeight)
        {
            for (int row = 0; row < ui->mapDataTableWidget->rowCount(); row++)
                ui->mapDataTableWidget->verticalHeader()->resizeSection(row, mapCellHeight);
        }
    */
    setMapTableWidgetSize(mdiAreaSize.width() - 15, mdiAreaSize.height() - 15, xSize);

    if (map.type != "Selectable" && map.type != "Switch")
    {
        connect(ui->mapDataTableWidget, SIGNAL(cellClicked(int, int)), this, SLOT(cellClicked(int, int)));
        connect(ui->mapDataTableWidget, SIGNAL(cellPressed(int, int)), this, SLOT(cellPressed(int, int)));
        /// connect(ui->mapDataTableWidget, SIGNAL(cellEntered(int, int)), this, SLOT (cellActivated(int, int)));
        connect(ui->mapDataTableWidget, SIGNAL(currentCellChanged(int, int, int, int)), this,
                SLOT(cellChanged(int, int, int, int)));
    }
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
        return;
    }
    const auto& map = *shown;
    const QSignalBlocker table_blocker(ui->mapDataTableWidget);
    QFont font = ui->mapDataTableWidget->font();
    font.setPointSize(cellFontSize);
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
            connect(checkbox, &QCheckBox::stateChanged, this, &CalibrationMaps::checkbox_state_changed);
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
                combo->setFixedWidth(mapCellWidthSelectable);
                combo->setObjectName("selectableComboBox");
                const QSignalBlocker blocker(combo);
                for (int i = 0; i < map.selection_names.size() - (multi ? 0 : 1); ++i)
                {
                    if (multi || !map.selection_names[i].isEmpty())
                    {
                        combo->addItem(map.selection_names[i]);
                    }
                }
                ui->mapDataTableWidget->setCellWidget(row, col, combo);
                connect(combo, &QComboBox::currentTextChanged, this,
                        &CalibrationMaps::selectable_combobox_item_changed);
            }
            const QSignalBlocker blocker(combo);
            int current = multi ? map.body.value(0).toInt() : 0;
            if (!multi)
            {
                for (int i = 0; i < map.selection_values.size() - 1; ++i)
                {
                    if (map.selection_values[i].toUpper() == map.body.join(',').toUpper())
                    {
                        current = i;
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

    const auto cell = [&](int row, int col, const QString& text, const QColor& background)
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
        item->setText(text);
    };
    const bool static_x = map.x_type == "Static Y Axis" || map.x_type == "Static X Axis";
    if (map.type != "1D")
    {
        if (map.y_size > 1 && (map.x_size <= 1 || !static_x))
        {
            for (int i = 0; i < map.y_size; ++i)
            {
                cell(i + ySizeOffset, 0, fastecu::ui::format_map_value(map.y_axis.value(i), map.y_format), Qt::white);
            }
        }
        QStringList axis = map.x_axis;
        for (int i = 0; i < map.x_size; ++i)
        {
            QString text;
            if (axis.value(i) == " ")
            {
                // Legacy inserts each fallback before the absent-axis sentinel.
                axis.insert(i, QString::number(i));
                text = axis[i];
            }
            else
            {
                text = static_x ? axis.value(i) : fastecu::ui::format_map_value(axis.value(i), map.x_format);
            }
            cell(0, i + xSizeOffset, text, Qt::white);
            const int width = QFontMetrics(font).horizontalAdvance(text) + 20;
            ui->mapDataTableWidget->horizontalHeader()->resizeSection(i + xSizeOffset, width);
        }
    }
    for (int i = 0; i < map.x_size * map.y_size; ++i)
    {
        const int row = i / map.x_size + ySizeOffset;
        const int col = i % map.x_size + xSizeOffset;
        cell(row, col, fastecu::ui::format_map_value(map.body.value(i), map.format),
             map.type == "1D" ? QColor(Qt::white)
                              : fastecu::ui::map_cell_color(map.body.value(i).toFloat(), color_bounds_));
    }
}

void CalibrationMaps::cellClicked(int row, int col)
{
    startCol = col;
    startRow = row;
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
    startCol = col;
    startRow = row;
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
    if (startCol == 0 && objectName.at(3) == "3D")
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
    else if (startRow == 0 &&
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
