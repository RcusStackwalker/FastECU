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
      ui_{std::make_unique<Ui::CalibrationMaps>()}
{
    ui_->setupUi(this);
    ui_->mapDataTableWidget->installEventFilter(this);
    this->setObjectName(fastecu::ui::sessionKeyText(session_) + "," + QString::number(map_index_) + ",,");
    this->setAttribute(Qt::WA_DeleteOnClose);
    ui_->mapNameLabel->clear();
    ui_->xScaleUnitsLabel->clear();
    ui_->mapDataUnitsLabel->clear();
    map_error_label_ = new QLabel(this);
    map_error_label_->setObjectName("mapDecodeError");
    map_error_label_->setTextFormat(Qt::PlainText);
    map_error_label_->setWordWrap(true);
    map_error_label_->hide();
    ui_->verticalLayout->addWidget(map_error_label_);
    const auto *rom = workspace_.Find(session_);
    if (rom != nullptr && rom->Definition() != nullptr && map_index_ >= 0 &&
        static_cast<std::size_t>(map_index_) < rom->Definition()->definition.maps.size())
    {
        const auto& name = rom->Definition()->definition.maps[static_cast<std::size_t>(map_index_)].name;
        setObjectName(fastecu::ui::sessionKeyText(session_) + "," + QString::number(map_index_) + "," +
                      QString::fromStdString(name));
        setWindowTitle(QString::fromStdString(name) + " - " + QString::fromStdString(rom->Source().display_name));
        ui_->mapNameLabel->setText(QString::fromStdString(name));
    }
    refresh();
}

namespace
{
// Selection maps show one control instead of numeric cells.
bool isSelectionType(const QString& type)
{
    return type == "Switch" || type == "MultiSelectable" || type == "Selectable";
}

bool isStaticAxis(const QString& type)
{
    return type == "Static Y Axis" || type == "Static X Axis";
}

fastecu::ui::PresentedCell axisCell(const std::optional<std::vector<fastecu::ui::PresentedCell>>& axis, int index)
{
    return axis.has_value() ? axis->at(static_cast<std::size_t>(index))
                            : fastecu::ui::PresentedCell{.text = QString::number(index)};
}

// A Selectable map shows the last selection whose value matches the blob; a
// MultiSelectable map shows the selection its first body cell names.
int selectedIndex(const fastecu::ui::MapPresentation& map, bool multi)
{
    if (multi)
    {
        return map.body.empty() ? 0 : fastecu::ui::selectionIndex(map.body.front());
    }
    int current = 0;
    for (std::size_t i = 0; map.blob.has_value() && i < map.selection_values.size(); ++i)
    {
        if (map.selection_values[i] == *map.blob)
        {
            current = static_cast<int>(i);
        }
    }
    return current;
}
} // namespace

void CalibrationMaps::initializeView(const fastecu::ui::MapPresentation& map,
                                     const fastecu::calibration::RomSource& source)
{
    this->setWindowTitle(map.name + " - " + QString::fromStdString(source.display_name));

    QString xScaleUnitsTitle;
    if (map.x_name != " ")
    {
        xScaleUnitsTitle = map.x_units != " " ? map.x_name + " (" + map.x_units + ")" : map.x_name;
    }
    ui_->xScaleUnitsLabel->setText(xScaleUnitsTitle);
    if (!map.units.isEmpty())
    {
        ui_->mapDataUnitsLabel->setText(map.units);
    }

    numeric_body_ = !isSelectionType(map.type);
    const QString mapWindowObjectName =
        fastecu::ui::sessionKeyText(session_) + "," + QString::number(map_index_) + "," + map.name + layoutTable(map);
    this->setObjectName(mapWindowObjectName);
    ui_->mapDataTableWidget->setObjectName(mapWindowObjectName);
    ui_->mapDataTableWidget->setColumnCount(x_size);
    ui_->mapDataTableWidget->setRowCount(y_size);
    ui_->mapDataTableWidget->setStyleSheet("QTableWidget::item { padding: 3px }");
    ui_->mapNameLabel->setText(map.name);

    if (map.type == "3D")
    {
        QTableWidgetItem *cellItem = new QTableWidgetItem;
        cellItem->setFlags(Qt::NoItemFlags);
        cellItem->setBackground(QBrush(QColor(0xf0, 0xf0, 0xf0, 255)));
        ui_->mapDataTableWidget->setItem(0, 0, cellItem);
    }

    if (map.type != "Selectable" && map.type != "Switch")
    {
        connect(ui_->mapDataTableWidget, &QTableWidget::cellClicked, this, &CalibrationMaps::startSelection,
                Qt::UniqueConnection);
        connect(ui_->mapDataTableWidget, &QTableWidget::cellPressed, this, &CalibrationMaps::startSelection,
                Qt::UniqueConnection);
        connect(ui_->mapDataTableWidget, &QTableWidget::currentCellChanged, this, &CalibrationMaps::cellChanged,
                Qt::UniqueConnection);
    }
    view_initialized_ = true;
}

// Sizes the table for the map type and returns its object-name suffix, which
// the cell slots read back.
QString CalibrationMaps::layoutTable(const fastecu::ui::MapPresentation& map)
{
    if (map.type == "2D")
    {
        return layout2D(map);
    }
    if (map.type == "3D")
    {
        return layout3D(map);
    }
    if (isSelectionType(map.type) || map.type == "1D")
    {
        map_cell_width = map.type == "1D" ? map_cell_width1_d : map_cell_width_selectable;
        x_size = 1;
        y_size = 1;
        ui_->xScaleUnitsLabel->setFixedHeight(0);
        return "," + map.type;
    }
    return {};
}

QString CalibrationMaps::layout2D(const fastecu::ui::MapPresentation& map)
{
    this->setWindowIcon(QIcon(map.y_size > 1 || map.x_size > 1 ? ":/icons/2D-64-W.png" : ":/icons/1D-64-W.png"));
    setAxisOffsets(map, isStaticAxis(map.x_type));
    if (map.x_type == "Static X Axis")
    {
        return ",Static X Axis";
    }
    // A Static Y Axis map also names its orientation.
    const QString orientation = map.y_size > 1 ? ",Y Axis" : ",X Axis";
    return map.x_type == "Static Y Axis" ? ",Static Y Axis" + orientation : orientation;
}

QString CalibrationMaps::layout3D(const fastecu::ui::MapPresentation& map)
{
    this->setWindowIcon(QIcon(":/icons/3D-64-W.png"));
    setAxisOffsets(map, false);

    auto *yScaleUnitsLabel = findChild<QLabel *>("mapYAxisUnits");
    if (yScaleUnitsLabel == nullptr)
    {
        yScaleUnitsLabel = new VerticalLabel();
        yScaleUnitsLabel->setObjectName("mapYAxisUnits");
        ui_->horizontalLayout_2->insertWidget(0, yScaleUnitsLabel);
    }
    yScaleUnitsLabel->setAlignment(Qt::AlignCenter);
    yScaleUnitsLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    yScaleUnitsLabel->setText(map.y_name + " (" + map.y_units + ")");
    return ",3D";
}

// A Y axis takes the first column and an X (or static) axis the first row.
void CalibrationMaps::setAxisOffsets(const fastecu::ui::MapPresentation& map, bool staticX)
{
    x_size_offset = map.y_size > 1 ? 1 : 0;
    y_size_offset = map.x_size > 1 || staticX ? 1 : 0;
    x_size = map.x_size + x_size_offset;
    y_size = map.y_size + y_size_offset;
}

void CalibrationMaps::showMapError(const fastecu::Error& error)
{
    const QSignalBlocker blocker(ui_->mapDataTableWidget);
    ui_->mapDataTableWidget->clear();
    ui_->mapDataTableWidget->setRowCount(0);
    ui_->mapDataTableWidget->setColumnCount(0);
    ui_->mapDataTableWidget->setEnabled(false);
    map_error_label_->setText(QString::fromStdString(error.detail));
    map_error_label_->show();
    view_initialized_ = false;
}

bool CalibrationMaps::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui_->mapDataTableWidget && event->type() == QEvent::KeyPress && numeric_body_ &&
        static_cast<QKeyEvent *>(event)->matches(QKeySequence::SelectAll))
    {
        const auto *rom = workspace_.Find(session_);
        const auto range = rom == nullptr
                               ? std::nullopt
                               : fastecu::ui::bodyWidgetRange(*rom, map_index_, ui_->mapDataTableWidget->rowCount(),
                                                              ui_->mapDataTableWidget->columnCount());
        if (range.has_value())
        {
            ui_->mapDataTableWidget->clearSelection();
            ui_->mapDataTableWidget->setRangeSelected(
                QTableWidgetSelectionRange(range->first_row, range->first_col, range->last_row, range->last_col), true);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

CalibrationMaps::~CalibrationMaps()
{
}

void CalibrationMaps::setMapTableWidgetSize(int maxWidth, int maxHeight, int xSizeArg)
{
    int w = 0;
    int h = 0;

    w += ui_->mapDataTableWidget->contentsMargins().left() + ui_->mapDataTableWidget->contentsMargins().right();
    h += ui_->mapDataTableWidget->contentsMargins().top() + ui_->mapDataTableWidget->contentsMargins().bottom();
    w += ui_->mapDataTableWidget->verticalHeader()->width();
    for (int i = 0; i < ui_->mapDataTableWidget->columnCount(); i++)
    {
        w += ui_->mapDataTableWidget->columnWidth(i);
    }
    for (int i = 0; i < ui_->mapDataTableWidget->rowCount(); i++)
    {
        h += ui_->mapDataTableWidget->rowHeight(i);
    }

    if (w > maxWidth)
    {
        w = maxWidth - 40;
        h += ui_->mapDataTableWidget->horizontalHeader()->height() + 15;
    }
    else
    {
        h += ui_->mapDataTableWidget->horizontalHeader()->height();
    }
    if (h > maxHeight)
    {
        h = maxHeight - 40;
        w += ui_->mapDataTableWidget->verticalHeader()->width() + 15;
    }
    else
    {
        w += ui_->mapDataTableWidget->verticalHeader()->width();
    }

    ui_->mapDataTableWidget->setMinimumWidth(w);
    ui_->mapDataTableWidget->setMaximumWidth(w);
    ui_->mapDataTableWidget->setMinimumHeight(h);
    ui_->mapDataTableWidget->setMaximumHeight(h);

    ui_->mapDataTableWidget->setFixedWidth(w);
    ui_->mapDataTableWidget->setFixedHeight(h);
}

void CalibrationMaps::refresh()
{
    const auto *rom = workspace_.Find(session_);
    if (rom == nullptr)
    {
        return;
    }
    const auto shown = fastecu::ui::presentMap(*rom, static_cast<std::size_t>(map_index_));
    if (!shown.has_value())
    {
        showMapError(shown.error());
        return;
    }
    const auto& map = *shown;
    map_error_label_->hide();
    ui_->mapDataTableWidget->setEnabled(true);
    const bool initialized = !view_initialized_;
    if (initialized)
    {
        initializeView(map, rom->Source());
    }
    if (!color_bounds_.has_value())
    {
        color_bounds_ = fastecu::ui::openingColorBounds(map);
    }
    const auto resize = qScopeGuard(
        [this, initialized]
        {
            if (initialized)
            {
                ui_->mapDataTableWidget->horizontalHeader()->resizeSections(QHeaderView::ResizeToContents);
                ui_->mapDataTableWidget->verticalHeader()->resizeSections(QHeaderView::ResizeToContents);
                setMapTableWidgetSize(mdi_area_size_.width() - 15, mdi_area_size_.height() - 15, x_size);
            }
        });
    const QSignalBlocker tableBlocker(ui_->mapDataTableWidget);
    QFont font = ui_->mapDataTableWidget->font();
    font.setPointSize(cell_font_size);
    font.setFamily("Franklin Gothic");

    if (map.type == "Switch")
    {
        refreshSwitch();
        return;
    }
    // The existing MultiSelectable layout has one visible column. Its numeric
    // rendering follows control creation and replaces the label.
    if ((map.type == "Selectable" || map.type == "MultiSelectable") && !refreshSelectable(map, font))
    {
        return;
    }
    refreshAxes(map, font);
    refreshBody(map, font);
}

// The retained typed formats resolve RomRaider switches to Selectable. No
// format populates legacy StateList; preserve the remaining empty switch
// control without introducing a new definition representation.
void CalibrationMaps::refreshSwitch()
{
    auto *checkbox = qobject_cast<QCheckBox *>(ui_->mapDataTableWidget->cellWidget(0, 0));
    if (checkbox == nullptr)
    {
        checkbox = new QCheckBox("On/Off");
        ui_->mapDataTableWidget->setCellWidget(0, 0, checkbox);
        connect(checkbox, &QCheckBox::checkStateChanged, this,
                [this](Qt::CheckState state) { emit checkboxStateChanged(static_cast<int>(state)); });
    }
    const QSignalBlocker blocker(checkbox);
    checkbox->setChecked(false);
}

// Returns whether numeric cells follow the controls (MultiSelectable).
bool CalibrationMaps::refreshSelectable(const fastecu::ui::MapPresentation& map, const QFont& font)
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
            ui_->mapDataTableWidget->setItem(row, 0, item);
        }
        auto *combo = selectableCombo(row, multi ? 1 : 0, map, font);
        const QSignalBlocker blocker(combo);
        combo->setCurrentIndex(selectedIndex(map, multi));
    }
    return multi;
}

QComboBox *CalibrationMaps::selectableCombo(int row, int col, const fastecu::ui::MapPresentation& map,
                                            const QFont& font)
{
    if (auto *combo = qobject_cast<QComboBox *>(ui_->mapDataTableWidget->cellWidget(row, col)))
    {
        return combo;
    }
    const bool multi = map.type == "MultiSelectable";
    auto *combo = new QComboBox;
    combo->setFont(font);
    combo->setFixedWidth(map_cell_width_selectable);
    combo->setObjectName("selectableComboBox");
    const QSignalBlocker blocker(combo);
    for (const auto& name : map.selection_names)
    {
        if (multi || !name.isEmpty())
        {
            combo->addItem(name);
        }
    }
    if (multi)
    {
        // Preserve the explicit empty choice of the existing control.
        combo->addItem(map.selection_names.isEmpty() ? " " : "");
    }
    ui_->mapDataTableWidget->setCellWidget(row, col, combo);
    connect(combo, &QComboBox::currentTextChanged, this, &CalibrationMaps::selectableComboboxItemChanged);
    return combo;
}

void CalibrationMaps::refreshAxes(const fastecu::ui::MapPresentation& map, const QFont& font)
{
    if (map.type == "1D")
    {
        return;
    }
    if (map.y_size > 1 && (map.x_size <= 1 || !isStaticAxis(map.x_type)))
    {
        for (int i = 0; i < map.y_size; ++i)
        {
            setCell(i + y_size_offset, 0, axisCell(map.y_axis, i), Qt::white, font);
        }
    }
    for (int i = 0; i < map.x_size; ++i)
    {
        const auto value = axisCell(map.x_axis, i);
        setCell(0, i + x_size_offset, value, Qt::white, font);
        const int width = QFontMetrics(font).horizontalAdvance(value.text) + 20;
        ui_->mapDataTableWidget->horizontalHeader()->resizeSection(i + x_size_offset, width);
    }
}

void CalibrationMaps::refreshBody(const fastecu::ui::MapPresentation& map, const QFont& font)
{
    for (int i = 0; i < map.x_size * map.y_size; ++i)
    {
        const int row = i / map.x_size + y_size_offset;
        const int col = i % map.x_size + x_size_offset;
        const auto& value = map.body.at(static_cast<std::size_t>(i));
        const auto background = map.type != "1D" && value.numeric_value.has_value() && color_bounds_.has_value()
                                    ? fastecu::ui::mapCellColor(*value.numeric_value, *color_bounds_)
                                    : QColor(Qt::white);
        setCell(row, col, value, background, font);
    }
}

void CalibrationMaps::setCell(int row, int col, const fastecu::ui::PresentedCell& value, const QColor& background,
                              const QFont& font)
{
    auto *item = ui_->mapDataTableWidget->item(row, col);
    if (item == nullptr)
    {
        item = new QTableWidgetItem;
        ui_->mapDataTableWidget->setItem(row, col, item);
    }
    item->setTextAlignment(Qt::AlignCenter);
    item->setFont(font);
    item->setForeground(Qt::black);
    item->setBackground(background);
    item->setText(value.text);
    item->setToolTip(value.diagnostic);
}

QString CalibrationMaps::mapLayout() const
{
    return ui_->mapDataTableWidget->objectName().split(",").at(3);
}

void CalibrationMaps::setItemFlags(int firstRow, int firstCol, Qt::ItemFlags flags)
{
    for (int col = firstCol; col < ui_->mapDataTableWidget->columnCount(); col++)
    {
        for (int row = firstRow; row < ui_->mapDataTableWidget->rowCount(); row++)
        {
            if (auto *item = ui_->mapDataTableWidget->item(row, col))
            {
                item->setFlags(flags);
            }
        }
    }
}

// Keeps the 3D corner and a static axis row out of every selection.
void CalibrationMaps::lockAxisCells(const QString& layout)
{
    if (layout == "3D")
    {
        ui_->mapDataTableWidget->item(0, 0)->setFlags(Qt::ItemIsEditable);
    }
    if (isStaticAxis(layout) && ui_->mapDataTableWidget->rowCount() > 1)
    {
        for (int col = 0; col < ui_->mapDataTableWidget->columnCount(); col++)
        {
            ui_->mapDataTableWidget->item(0, col)->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled);
        }
    }
}

void CalibrationMaps::startSelection(int row, int col)
{
    start_col = col;
    start_row = row;
    setItemFlags(0, 0, Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsEnabled);
    ui_->mapDataTableWidget->item(row, col)->setSelected(true);
    lockAxisCells(mapLayout());
}

// While a selection extends, cells outside the part it started in (axis row,
// axis column or body) stop being selectable.
void CalibrationMaps::cellChanged(int curRow, int curCol, int prevRow, int prevCol)
{
    const QString layout = mapLayout();
    const Qt::ItemFlags fixed = Qt::ItemIsEditable | Qt::ItemIsEnabled;
    if (start_col == 0 && layout == "3D")
    {
        setItemFlags(0, 1, fixed);
    }
    else if (start_row == 0 && (layout == "3D" || layout == "X Axis" || layout == "Y Axis"))
    {
        setItemFlags(1, 0, fixed);
    }
    else
    {
        releaseAxisCells(layout);
    }
    lockAxisCells(layout);
}

void CalibrationMaps::releaseAxisCells(const QString& layout)
{
    const auto release = [this](int row, int col)
    {
        auto *item = ui_->mapDataTableWidget->item(row, col);
        item->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled);
        item->setSelected(false);
    };
    if (layout == "3D" || layout == "X Axis")
    {
        for (int col = 0; col < ui_->mapDataTableWidget->columnCount(); col++)
        {
            release(0, col);
        }
    }
    if (layout == "3D")
    {
        for (int row = 0; row < ui_->mapDataTableWidget->rowCount(); row++)
        {
            release(row, 0);
        }
    }
}
