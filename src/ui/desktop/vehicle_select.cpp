#include "vehicle_select.h"
#include "ui_vehicle_select.h"

#include <algorithm>
#include <functional>

#include "src/ui/desktop/config_fields.h"

using fastecu::config::ProtocolEntry;
using fastecu::config::ResolvedCarModel;
using fastecu::ui::protocol_field;
using fastecu::ui::qs;

VehicleSelect::VehicleSelect(const fastecu::config::ConfigSession& config, QWidget *parent)
    : QDialog(parent), config(config), ui{std::make_unique<Ui::VehicleSelect>()}
{
    ui->setupUi(this);

    ui->select_button->setEnabled(false);

    ui->car_make_tree_widget->setHeaderLabel("Manufacturer");
    ui->car_model_tree_widget->setHeaderLabel("Model");
    QStringList car_version_tree_widget_headers = {"Version", "Type", "kW",  "HP", "Fuel", "Year",
                                                   "ECU",     "Mode", "CHK", "RD", "WR",   "Protocol"};
    ui->car_version_tree_widget->setHeaderLabels(car_version_tree_widget_headers);
    ui->car_version_tree_widget->setColumnWidth(0, 150);
    ui->car_version_tree_widget->setColumnWidth(1, 75);
    ui->car_version_tree_widget->setColumnWidth(2, 50);
    ui->car_version_tree_widget->setColumnWidth(3, 50);
    ui->car_version_tree_widget->setColumnWidth(4, 50);
    ui->car_version_tree_widget->setColumnWidth(5, 50);
    ui->car_version_tree_widget->setColumnWidth(6, 150);
    ui->car_version_tree_widget->setColumnWidth(7, 50);
    ui->car_version_tree_widget->setColumnWidth(8, 40);
    ui->car_version_tree_widget->setColumnWidth(9, 40);
    ui->car_version_tree_widget->setColumnWidth(10, 40);
    ui->car_version_tree_widget->setColumnWidth(11, 225);

    int width = 0;
    for (int i = 0; i < ui->car_version_tree_widget->columnCount(); i++)
    {
        width += ui->car_version_tree_widget->columnWidth(i);
    }

    // qDebug() << "Full width =" << width;
    ui->car_version_tree_widget->setMinimumWidth(width);

    int height = width / 4.0 * 2.5 + 18;
    this->setFixedHeight(height);

    font = ui->car_make_tree_widget->font();
    font.setPointSize(header_font_size);
    font.setBold(header_font_bold);
    font.setFamily(header_font_family);
    ui->car_make_tree_widget->setFont(font);
    ui->car_model_tree_widget->setFont(font);
    ui->car_version_tree_widget->setFont(font);

    const ResolvedCarModel *selected = config.selected_vehicle();
    const QString selected_make = selected != nullptr ? qs(selected->make) : QString();

    QStringList car_makes;
    QStringList car_makes_sorted;
    bool car_make_changed_saved = false;

    for (const ResolvedCarModel& vehicle : config.vehicles())
    {
        if (!car_makes.contains(qs(vehicle.make)))
        {
            car_makes.append(qs(vehicle.make));
        }
    }

    car_makes_sorted = car_makes;
    std::sort(car_makes_sorted.begin(), car_makes_sorted.end(), std::less<QString>());
    for (int i = 0; i < car_makes_sorted.length(); i++)
    {
        QTreeWidgetItem *item = new QTreeWidgetItem();
        item->setFont(0, font);
        item->setText(0, car_makes_sorted.at(i));
        item->setFirstColumnSpanned(true);
        ui->car_make_tree_widget->addTopLevelItem(item);
        if (car_makes_sorted.at(i) == selected_make)
        {
            car_make_changed_saved = true;
            ui->car_make_tree_widget->setCurrentItem(item);
        }
    }
    if (!car_make_changed_saved)
    {
        qDebug() << "Car make changed to first item, no make selected previously";
        QTreeWidgetItem *item = ui->car_model_tree_widget->topLevelItem(0);
        ui->car_model_tree_widget->setCurrentItem(item);
    }

    connect(ui->car_make_tree_widget, &QTreeWidget::itemSelectionChanged, this,
            &VehicleSelect::car_make_treewidget_item_selected);
    connect(ui->car_model_tree_widget, &QTreeWidget::itemSelectionChanged, this,
            &VehicleSelect::car_model_treewidget_item_selected);
    connect(ui->car_version_tree_widget, &QTreeWidget::itemSelectionChanged, this,
            &VehicleSelect::car_version_treewidget_item_selected);
    connect(ui->car_version_tree_widget, &QTreeWidget::itemDoubleClicked, this, &VehicleSelect::car_model_selected);
    connect(ui->cancel_button, &QPushButton::clicked, this, &QDialog::close);
    connect(ui->select_button, &QPushButton::clicked, this, &VehicleSelect::car_model_selected);

    const QModelIndex make_index = ui->car_make_tree_widget->selectionModel()->currentIndex();
    ui->car_make_tree_widget->setCurrentIndex(make_index);
    emit ui->car_make_tree_widget->itemSelectionChanged();

    // this->adjustSize();
    /*
        const QModelIndex model_index = ui->car_model_tree_widget->selectionModel()->currentIndex();
        ui->car_model_tree_widget->setCurrentIndex(model_index);
        emit ui->car_model_tree_widget->itemSelectionChanged();

        const QModelIndex version_index = ui->car_version_tree_widget->selectionModel()->currentIndex();
        ui->car_version_tree_widget->setCurrentIndex(version_index);
        emit ui->car_version_tree_widget->itemSelectionChanged();
    */
}

VehicleSelect::~VehicleSelect()
{
}

void VehicleSelect::car_model_selected()
{
    // Tentative: the caller applies an accepted choice to the session.
    bool parsed = false;
    const qulonglong row = flash_protocol_id.toULongLong(&parsed);
    if (parsed)
    {
        chosenRow = static_cast<std::size_t>(row);
    }
    accept();

    close();
}

std::optional<std::size_t> VehicleSelect::chosen_row() const
{
    return chosenRow;
}

void VehicleSelect::car_make_treewidget_item_selected()
{
    qDebug() << "Car make selection changed";
    QTreeWidgetItem *item = ui->car_make_tree_widget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *item_local = ui->car_make_tree_widget->selectedItems().at(0);
        QString selected_text = item_local->text(0);

        // qDebug() << "Check models for manufacturer:" << selected_text;

        const QString& car_make = selected_text;
        flash_protocol_id.clear();
        flash_protocol_make = car_make;
        flash_protocol_model.clear();
        flash_protocol_version.clear();

        QStringList car_models;
        QStringList car_models_sorted;
        bool car_model_changed_saved = false;

        // To delete treewidget items, disconnect itemSelectionChanged() signal first
        disconnect(ui->car_model_tree_widget, &QTreeWidget::itemSelectionChanged, 0, 0);
        qDebug() << "Delete car_model_tree_widget items";
        int item_count = ui->car_model_tree_widget->topLevelItemCount();
        for (int i = 0; i < item_count; i++)
        {
            QTreeWidgetItem *item_local = ui->car_model_tree_widget->topLevelItem(0);
            delete item_local;
        }
        // Connect itemSelectionChanged() signal again
        connect(ui->car_model_tree_widget, &QTreeWidget::itemSelectionChanged, this,
                &VehicleSelect::car_model_treewidget_item_selected);

        qDebug() << "Add models data based on selected make";
        for (const ResolvedCarModel& vehicle : config.vehicles())
        {
            const QString model = qs(vehicle.model);
            if (!car_models.contains(model) && qs(vehicle.make) == car_make && !model.isEmpty())
            {
                car_models.append(model);
            }
        }
        qDebug() << "Sort models data items alphabetically";
        car_models_sorted = car_models;
        std::sort(car_models_sorted.begin(), car_models_sorted.end(), std::less<QString>());

        qDebug() << "Add models data items to select";
        for (int i = 0; i < car_models_sorted.length(); i++)
        {
            QTreeWidgetItem *item_local = new QTreeWidgetItem();
            item_local->setText(0, car_models_sorted.at(i));
            item_local->setFirstColumnSpanned(true);
            ui->car_model_tree_widget->addTopLevelItem(item_local);
            if (config.selected_vehicle() != nullptr && car_models_sorted.at(i) == qs(config.selected_vehicle()->model))
            {
                qDebug() << "Car model changed to saved model";
                car_model_changed_saved = true;
                ui->car_model_tree_widget->setCurrentItem(item_local);
            }
        }
        if (!car_model_changed_saved)
        {
            qDebug() << "Car model changed to first item, no model selected previously";
            QTreeWidgetItem *item_local = ui->car_model_tree_widget->topLevelItem(0);
            ui->car_model_tree_widget->setCurrentItem(item_local);
        }
    }
    qDebug() << "Car make selection applied";
}

void VehicleSelect::car_model_treewidget_item_selected()
{
    qDebug() << "Car model selection changed";
    QTreeWidgetItem *item = ui->car_model_tree_widget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *item_local = ui->car_model_tree_widget->selectedItems().at(0);
        QString selected_text = item_local->text(0);

        // qDebug() << "Check versions for model:" << selected_text;

        const QString& car_model = selected_text;
        QStringList car_versions;
        QStringList car_versions_sorted;
        bool car_version_changed_saved = false;

        flash_protocol_id.clear();
        flash_protocol_model = car_model;
        flash_protocol_version.clear();

        QStringList id;
        QStringList version;
        QStringList type;
        QStringList kw;
        QStringList hp;
        QStringList fuel;
        QStringList year;
        QStringList ecu;
        QStringList mcu;
        QStringList mode;
        QStringList checksum;
        QStringList read;
        QStringList write;
        QStringList family;
        QStringList description;

        // To delete treewidget items, disconnect itemSelectionChanged() signal first
        disconnect(ui->car_version_tree_widget, &QTreeWidget::itemSelectionChanged, 0, 0);
        qDebug() << "Delete car_version_tree_widget items";
        int item_count = ui->car_version_tree_widget->topLevelItemCount();
        for (int i = 0; i < item_count; i++)
        {
            QTreeWidgetItem *item_local = ui->car_version_tree_widget->topLevelItem(0);
            delete item_local;
        }
        // Connect itemSelectionChanged() signal again
        connect(ui->car_version_tree_widget, &QTreeWidget::itemSelectionChanged, this,
                &VehicleSelect::car_version_treewidget_item_selected);

        qDebug() << "Add versions data based on selected model";
        const auto vehicles = config.vehicles();
        for (std::size_t i = 0; i < vehicles.size(); i++)
        {
            const ResolvedCarModel& vehicle = vehicles[i];
            if (qs(vehicle.model) == car_model && qs(vehicle.make) == flash_protocol_make)
            {
                // A row's id is its catalog position.
                id.append(QString::number(i));
                version.append(qs(vehicle.version));
                type.append(qs(vehicle.type));
                kw.append(qs(vehicle.kw));
                hp.append(qs(vehicle.hp));
                fuel.append(qs(vehicle.fuel));
                year.append(qs(vehicle.year));
                ecu.append(protocol_field(vehicle, &ProtocolEntry::ecu));
                mcu.append(protocol_field(vehicle, &ProtocolEntry::mcu));
                mode.append(protocol_field(vehicle, &ProtocolEntry::mode));
                checksum.append(protocol_field(vehicle, &ProtocolEntry::checksum));
                read.append(protocol_field(vehicle, &ProtocolEntry::read));
                write.append(protocol_field(vehicle, &ProtocolEntry::write));
                family.append(qs(vehicle.protocol_name));
                description.append(protocol_field(vehicle, &ProtocolEntry::description));
            }
        }

        qDebug() << "Add versions data items to select";
        for (int i = 0; i < version.length(); i++)
        {
            QTreeWidgetItem *item_local = new QTreeWidgetItem();

            item_local->setText(0, version.at(i));
            item_local->setText(1, type.at(i));
            item_local->setText(2, kw.at(i));
            item_local->setText(3, hp.at(i));
            item_local->setText(4, fuel.at(i));
            item_local->setText(5, year.at(i));
            item_local->setText(6, ecu.at(i));
            item_local->setText(7, mode.at(i));
            if (checksum.at(i) == "yes")
            {
                item_local->setCheckState(8, Qt::Checked);
                item_local->setForeground(8, Qt::darkGreen);
                item_local->setToolTip(8, "Checksum calculation supported");
            }
            else if (checksum.at(i) == "no")
            {
                item_local->setCheckState(8, Qt::Unchecked);
                item_local->setForeground(8, Qt::gray);
                item_local->setToolTip(8, "ROM has no checksum");
            }
            else if (checksum.at(i) == "n/a")
            {
                item_local->setCheckState(8, Qt::Checked);
                item_local->setForeground(8, Qt::red);
                item_local->setToolTip(8, "Checksum calculation NOT supported yet");
            }
            if (read.at(i) == "yes")
            {
                item_local->setCheckState(9, Qt::Checked);
            }
            else
            {
                item_local->setCheckState(9, Qt::Unchecked);
            }
            if (write.at(i) == "yes")
            {
                item_local->setCheckState(10, Qt::Checked);
            }
            else
            {
                item_local->setCheckState(10, Qt::Unchecked);
            }

            item_local->setText(11, family.at(i));
            item_local->setToolTip(11, family.at(i));
            item_local->setText(12, id.at(i));
            item_local->setText(13, description.at(i));
            // topLevelCarVersionTreeItem->setFirstColumnSpanned(true);
            ui->car_version_tree_widget->addTopLevelItem(item_local);

            qDebug() << "Check if car version selected";
            if (id.at(i) == qs(config.settings().selected_protocol_id))
            {
                qDebug() << "Car version changed to saved model";
                car_version_changed_saved = true;
                ui->car_version_tree_widget->setCurrentItem(item_local);
            }
        }
        if (!car_version_changed_saved)
        {
            qDebug() << "Car version changed to first item, no version selected previously";
            QTreeWidgetItem *item_local = ui->car_version_tree_widget->topLevelItem(0);
            ui->car_version_tree_widget->setCurrentItem(item_local);
        }
    }
    qDebug() << "Car model selection applied";
}

void VehicleSelect::car_version_treewidget_item_selected()
{
    QTreeWidgetItem *item = ui->car_version_tree_widget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *item_local = ui->car_version_tree_widget->selectedItems().at(0);
        QString selected_text = item_local->text(0);

        // qDebug() << "Selected version for model" << flash_protocol_model << "is" << selected_text;

        ui->select_button->setEnabled(true);

        const QString& car_version = selected_text;
        flash_protocol_version.clear();
        flash_protocol_version = car_version;
        flash_protocol_family = item_local->text(11);
        flash_protocol_id = item_local->text(12);
        flash_protocol_description = item_local->text(13);

        // flash_protocol_id = selected_text;//car_version_treewidget_item->text(11);

        // qDebug() << "Protocol ID:" << flash_protocol_id;
    }
    qDebug() << "Car version selection applied";
}
