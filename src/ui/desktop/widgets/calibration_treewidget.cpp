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

    const QStringList labels = fastecu::ui::romInfoLabels();
    const QStringList values = fastecu::ui::romInfoValues(session, view.missing_definition_make);
    QTreeWidgetItem *romInfoItem = new QTreeWidgetItem();
    romInfoItem->setText(0, "ROM Info");
    calDataTree->addTopLevelItem(romInfoItem);
    if (view.rom_info_expanded)
    {
        romInfoItem->setExpanded(true);
    }
    for (int i = 0; i < values.length(); i++)
    {
        emit logD("Set " + labels.at(i) + ": " + values.at(i), true, true);
        QTreeWidgetItem *item = new QTreeWidgetItem();
        item->setText(0, labels.at(i) + ": " + values.at(i));
        calDataTree->topLevelItem(0)->addChild(item);
    }

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
        bool treeCategoryCreated = false;
        for (int i = 0; i < calDataTree->topLevelItemCount(); i++)
        {
            if (calDataTree->topLevelItem(i)->text(0) == category)
            {
                treeCategoryCreated = true;
            }
        }
        if (!treeCategoryCreated)
        {
            QTreeWidgetItem *categoryItem = new QTreeWidgetItem();
            categoryItem->setText(0, category);
            calDataTree->addTopLevelItem(categoryItem);
            if (view.expanded_categories.contains(category))
            {
                categoryItem->setExpanded(true);
            }
        }
        const QString type = legacyValue(map.type);
        for (int i = 0; i < calDataTree->topLevelItemCount(); i++)
        {
            if (calDataTree->topLevelItem(i)->text(0) != category)
            {
                continue;
            }
            QTreeWidgetItem *item = new QTreeWidgetItem();
            if (type == "1D" || type == "Selectable" || (map.y_size == 1 && map.x_size == 1))
            {
                item->setIcon(0, QIcon(":/icons/1D-64.png"));
            }
            else if (type == "2D")
            {
                item->setIcon(0, QIcon(":/icons/2D-64.png"));
            }
            else if (type == "3D")
            {
                item->setIcon(0, QIcon(":/icons/3D-64.png"));
            }
            item->setCheckState(0, view.open_maps.contains(j) ? Qt::Checked : Qt::Unchecked);
            item->setText(0, name);
            item->setText(1, QString::number(j));
            calDataTree->topLevelItem(i)->addChild(item);
            item->setToolTip(0, name + legacyValue(map.description));
        }
    }

    return calDataTree;
}
