#include "src/ui/desktop/widgets/calibration_treewidget.h"

#include "src/ui/desktop/calibration/rom_info.h"

CalibrationTreeWidget::CalibrationTreeWidget()
{
}

namespace
{

QString legacyValue(const std::string& value)
{
    return value.empty() ? QString(" ") : QString::fromStdString(value);
}

// Finds the top-level item of a category, creating it (expanded as the view
// had it) when it is missing.
QTreeWidgetItem *categoryItem(QTreeWidget *tree, const QString& category, const fastecu::ui::CalibrationViewState& view)
{
    for (int i = 0; i < tree->topLevelItemCount(); i++)
    {
        if (tree->topLevelItem(i)->text(0) == category)
        {
            return tree->topLevelItem(i);
        }
    }
    auto *item = new QTreeWidgetItem();
    item->setText(0, category);
    tree->addTopLevelItem(item);
    if (view.expanded_categories.contains(category))
    {
        item->setExpanded(true);
    }
    return item;
}

QString mapIcon(const fastecu::definition::CalibrationMap& map)
{
    const QString type = legacyValue(map.type);
    if (type == "1D" || type == "Selectable" || (map.y_size == 1 && map.x_size == 1))
    {
        return ":/icons/1D-64.png";
    }
    if (type == "2D")
    {
        return ":/icons/2D-64.png";
    }
    if (type == "3D")
    {
        return ":/icons/3D-64.png";
    }
    return {};
}

} // namespace

QTreeWidget *CalibrationTreeWidget::buildCalibrationFilesTree(fastecu::calibration::SessionId sessionId,
                                                              QTreeWidget *filesTreeWidget,
                                                              const fastecu::calibration::CalibrationSession& session)
{
    QTreeWidget *calFilesTree = filesTreeWidget;
    calFilesTree->setAnimated(true);
    calFilesTree->setFocusPolicy(Qt::NoFocus);
    QFont filesItemFont;
    filesItemFont.setPixelSize(14);
    calFilesTree->setFont(filesItemFont);

    QTreeWidgetItem *topLevelFilesTreeItem = new QTreeWidgetItem();
    topLevelFilesTreeItem->setText(2, fastecu::ui::sessionKeyText(sessionId));
    topLevelFilesTreeItem->setCheckState(0, Qt::Unchecked);
    topLevelFilesTreeItem->setFirstColumnSpanned(true);
    calFilesTree->addTopLevelItem(topLevelFilesTreeItem);

    topLevelFilesTreeItem->setText(0, QString::fromStdString(session.Source().display_name));
    if (const auto *resolved = session.Definition(); resolved != nullptr && !resolved->definition.maps.empty())
    {
        topLevelFilesTreeItem->setText(1, legacyValue(resolved->definition.maps.front().id));
    }
    calFilesTree->expandItem(topLevelFilesTreeItem);

    for (int i = 0; i < calFilesTree->topLevelItemCount(); i++)
    {
        calFilesTree->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
        calFilesTree->topLevelItem(i)->setSelected(false);
    }
    topLevelFilesTreeItem->setSelected(true);
    topLevelFilesTreeItem->setCheckState(0, Qt::Checked);

    return calFilesTree;
}

QTreeWidget *CalibrationTreeWidget::buildCalibrationDataTree(QTreeWidget *dataTreeWidget,
                                                             const fastecu::calibration::CalibrationSession& session,
                                                             const fastecu::ui::CalibrationViewState& view)
{
    dataTreeWidget->clear();

    QTreeWidget *calDataTree = dataTreeWidget;
    calDataTree->setAnimated(true);
    calDataTree->setFocusPolicy(Qt::NoFocus);
    QFont dataItemFont;
    dataItemFont.setPixelSize(12);
    calDataTree->setFont(dataItemFont);
    addRomInfo(calDataTree, session, view);

    const fastecu::calibration::ResolvedDefinition *resolved = session.Definition();
    const std::size_t mapCount = resolved != nullptr ? resolved->definition.maps.size() : 0;
    for (std::size_t j = 0; j < mapCount; j++)
    {
        const fastecu::definition::CalibrationMap& map = resolved->definition.maps[j];
        const QString category = legacyValue(map.category);
        const QString name = legacyValue(map.name);
        if (category == " " || name == " ")
        {
            continue;
        }
        QTreeWidgetItem *parent = categoryItem(calDataTree, category, view);
        QTreeWidgetItem *item = new QTreeWidgetItem();
        if (const QString icon = mapIcon(map); !icon.isEmpty())
        {
            item->setIcon(0, QIcon(icon));
        }
        item->setCheckState(0, view.open_maps.contains(j) ? Qt::Checked : Qt::Unchecked);
        item->setText(0, name);
        item->setText(1, QString::number(j));
        parent->addChild(item);
        item->setToolTip(0, name + legacyValue(map.description));
    }

    return calDataTree;
}

void CalibrationTreeWidget::addRomInfo(QTreeWidget *tree, const fastecu::calibration::CalibrationSession& session,
                                       const fastecu::ui::CalibrationViewState& view)
{
    const QStringList labels = fastecu::ui::romInfoLabels();
    const QStringList values = fastecu::ui::romInfoValues(session, view.missing_definition_make);
    QTreeWidgetItem *romInfoItem = new QTreeWidgetItem();
    romInfoItem->setText(0, "ROM Info");
    tree->addTopLevelItem(romInfoItem);
    if (view.rom_info_expanded)
    {
        romInfoItem->setExpanded(true);
    }
    for (int i = 0; i < values.length(); i++)
    {
        emit logD("Set " + labels.at(i) + ": " + values.at(i), true, true);
        QTreeWidgetItem *item = new QTreeWidgetItem();
        item->setText(0, labels.at(i) + ": " + values.at(i));
        romInfoItem->addChild(item);
    }
}
