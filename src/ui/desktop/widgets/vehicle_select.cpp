#include "src/ui/desktop/widgets/vehicle_select.h"
#include "ui_vehicle_select.h"

#include <algorithm>
#include <functional>

#include "src/ui/desktop/config_fields.h"

using fastecu::config::ProtocolSpec;
using fastecu::config::VehicleSpec;
using fastecu::ui::checksumField;
using fastecu::ui::protocolField;
using fastecu::ui::protocolFlag;
using fastecu::ui::qs;

VehicleSelect::VehicleSelect(const fastecu::config::ConfigSession& config, QWidget *parent)
    : QDialog(parent), config_(config), ui_{std::make_unique<Ui::VehicleSelect>()}
{
    ui_->setupUi(this);

    ui_->select_button->setEnabled(false);

    ui_->car_make_tree_widget->setHeaderLabel("Manufacturer");
    ui_->car_model_tree_widget->setHeaderLabel("Model");
    QStringList carVersionTreeWidgetHeaders = {"Version", "Type", "kW",  "HP", "Fuel", "Year",
                                               "ECU",     "Mode", "CHK", "RD", "WR",   "Protocol"};
    ui_->car_version_tree_widget->setHeaderLabels(carVersionTreeWidgetHeaders);
    ui_->car_version_tree_widget->setColumnWidth(0, 150);
    ui_->car_version_tree_widget->setColumnWidth(1, 75);
    ui_->car_version_tree_widget->setColumnWidth(2, 50);
    ui_->car_version_tree_widget->setColumnWidth(3, 50);
    ui_->car_version_tree_widget->setColumnWidth(4, 50);
    ui_->car_version_tree_widget->setColumnWidth(5, 50);
    ui_->car_version_tree_widget->setColumnWidth(6, 150);
    ui_->car_version_tree_widget->setColumnWidth(7, 50);
    ui_->car_version_tree_widget->setColumnWidth(8, 40);
    ui_->car_version_tree_widget->setColumnWidth(9, 40);
    ui_->car_version_tree_widget->setColumnWidth(10, 40);
    ui_->car_version_tree_widget->setColumnWidth(11, 225);

    int width = 0;
    for (int i = 0; i < ui_->car_version_tree_widget->columnCount(); i++)
    {
        width += ui_->car_version_tree_widget->columnWidth(i);
    }

    // qDebug() << "Full width =" << width;
    ui_->car_version_tree_widget->setMinimumWidth(width);

    const auto height = static_cast<int>(width / 4.0 * 2.5 + 18);
    this->setFixedHeight(height);

    font_ = ui_->car_make_tree_widget->font();
    font_.setPointSize(header_font_size_);
    font_.setBold(header_font_bold_);
    font_.setFamily(header_font_family_);
    ui_->car_make_tree_widget->setFont(font_);
    ui_->car_model_tree_widget->setFont(font_);
    ui_->car_version_tree_widget->setFont(font_);

    const VehicleSpec *selected = config.SelectedVehicle();
    const QString selectedMake = selected != nullptr ? qs(selected->make) : QString();

    QStringList carMakes;
    QStringList carMakesSorted;
    bool carMakeChangedSaved = false;

    for (const VehicleSpec& vehicle : config.Vehicles())
    {
        if (!carMakes.contains(qs(vehicle.make)))
        {
            carMakes.append(qs(vehicle.make));
        }
    }

    carMakesSorted = carMakes;
    std::sort(carMakesSorted.begin(), carMakesSorted.end(), std::less<QString>());
    for (int i = 0; i < carMakesSorted.length(); i++)
    {
        QTreeWidgetItem *item = new QTreeWidgetItem();
        item->setFont(0, font_);
        item->setText(0, carMakesSorted.at(i));
        item->setFirstColumnSpanned(true);
        ui_->car_make_tree_widget->addTopLevelItem(item);
        if (carMakesSorted.at(i) == selectedMake)
        {
            carMakeChangedSaved = true;
            ui_->car_make_tree_widget->setCurrentItem(item);
        }
    }
    if (!carMakeChangedSaved)
    {
        qDebug() << "Car make changed to first item, no make selected previously";
        QTreeWidgetItem *item = ui_->car_make_tree_widget->topLevelItem(0);
        ui_->car_make_tree_widget->setCurrentItem(item);
    }

    connect(ui_->car_make_tree_widget, &QTreeWidget::itemSelectionChanged, this,
            &VehicleSelect::carMakeTreewidgetItemSelected);
    connect(ui_->car_model_tree_widget, &QTreeWidget::itemSelectionChanged, this,
            &VehicleSelect::carModelTreewidgetItemSelected);
    connect(ui_->car_version_tree_widget, &QTreeWidget::itemSelectionChanged, this,
            &VehicleSelect::carVersionTreewidgetItemSelected);
    connect(ui_->car_version_tree_widget, &QTreeWidget::itemDoubleClicked, this, &VehicleSelect::carModelSelected);
    connect(ui_->cancel_button, &QPushButton::clicked, this, &QDialog::close);
    connect(ui_->select_button, &QPushButton::clicked, this, &VehicleSelect::carModelSelected);

    const QModelIndex makeIndex = ui_->car_make_tree_widget->selectionModel()->currentIndex();
    ui_->car_make_tree_widget->setCurrentIndex(makeIndex);
    emit ui_->car_make_tree_widget->itemSelectionChanged();

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

void VehicleSelect::carModelSelected()
{
    // Tentative: the caller applies an accepted choice to the session.
    bool parsed = false;
    const qulonglong row = flash_protocol_id_.toULongLong(&parsed);
    if (parsed)
    {
        chosen_row_ = static_cast<std::size_t>(row);
    }
    accept();

    close();
}

std::optional<std::size_t> VehicleSelect::chosenRow() const
{
    return chosen_row_;
}

void VehicleSelect::carMakeTreewidgetItemSelected()
{
    qDebug() << "Car make selection changed";
    QTreeWidgetItem *item = ui_->car_make_tree_widget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *itemLocal = ui_->car_make_tree_widget->selectedItems().at(0);
        QString selectedText = itemLocal->text(0);

        // qDebug() << "Check models for manufacturer:" << selected_text;

        const QString& carMake = selectedText;
        flash_protocol_id_.clear();
        flash_protocol_make_ = carMake;
        flash_protocol_model_.clear();
        flash_protocol_version_.clear();

        QStringList carModels;
        QStringList carModelsSorted;
        bool carModelChangedSaved = false;

        // To delete treewidget items, disconnect itemSelectionChanged() signal first
        disconnect(ui_->car_model_tree_widget, &QTreeWidget::itemSelectionChanged, 0, 0);
        qDebug() << "Delete car_model_tree_widget items";
        int itemCount = ui_->car_model_tree_widget->topLevelItemCount();
        for (int i = 0; i < itemCount; i++)
        {
            QTreeWidgetItem *itemLocal = ui_->car_model_tree_widget->topLevelItem(0);
            delete itemLocal;
        }
        // Connect itemSelectionChanged() signal again
        connect(ui_->car_model_tree_widget, &QTreeWidget::itemSelectionChanged, this,
                &VehicleSelect::carModelTreewidgetItemSelected);

        qDebug() << "Add models data based on selected make";
        for (const VehicleSpec& vehicle : config_.Vehicles())
        {
            const QString model = qs(vehicle.model);
            if (!carModels.contains(model) && qs(vehicle.make) == carMake && !model.isEmpty())
            {
                carModels.append(model);
            }
        }
        qDebug() << "Sort models data items alphabetically";
        carModelsSorted = carModels;
        std::sort(carModelsSorted.begin(), carModelsSorted.end(), std::less<QString>());

        qDebug() << "Add models data items to select";
        for (int i = 0; i < carModelsSorted.length(); i++)
        {
            QTreeWidgetItem *itemLocal = new QTreeWidgetItem();
            itemLocal->setText(0, carModelsSorted.at(i));
            itemLocal->setFirstColumnSpanned(true);
            ui_->car_model_tree_widget->addTopLevelItem(itemLocal);
            if (config_.SelectedVehicle() != nullptr && carModelsSorted.at(i) == qs(config_.SelectedVehicle()->model))
            {
                qDebug() << "Car model changed to saved model";
                carModelChangedSaved = true;
                ui_->car_model_tree_widget->setCurrentItem(itemLocal);
            }
        }
        if (!carModelChangedSaved)
        {
            qDebug() << "Car model changed to first item, no model selected previously";
            QTreeWidgetItem *itemLocal = ui_->car_model_tree_widget->topLevelItem(0);
            ui_->car_model_tree_widget->setCurrentItem(itemLocal);
        }
    }
    qDebug() << "Car make selection applied";
}

void VehicleSelect::carModelTreewidgetItemSelected()
{
    qDebug() << "Car model selection changed";
    QTreeWidgetItem *item = ui_->car_model_tree_widget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *itemLocal = ui_->car_model_tree_widget->selectedItems().at(0);
        QString selectedText = itemLocal->text(0);

        // qDebug() << "Check versions for model:" << selected_text;

        const QString& carModel = selectedText;
        QStringList carVersions;
        QStringList carVersionsSorted;
        bool carVersionChangedSaved = false;

        flash_protocol_id_.clear();
        flash_protocol_model_ = carModel;
        flash_protocol_version_.clear();

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
        disconnect(ui_->car_version_tree_widget, &QTreeWidget::itemSelectionChanged, 0, 0);
        qDebug() << "Delete car_version_tree_widget items";
        int itemCount = ui_->car_version_tree_widget->topLevelItemCount();
        for (int i = 0; i < itemCount; i++)
        {
            QTreeWidgetItem *itemLocal = ui_->car_version_tree_widget->topLevelItem(0);
            delete itemLocal;
        }
        // Connect itemSelectionChanged() signal again
        connect(ui_->car_version_tree_widget, &QTreeWidget::itemSelectionChanged, this,
                &VehicleSelect::carVersionTreewidgetItemSelected);

        qDebug() << "Add versions data based on selected model";
        const auto vehicles = config_.Vehicles();
        for (std::size_t i = 0; i < vehicles.size(); i++)
        {
            const VehicleSpec& vehicle = vehicles[i];
            if (qs(vehicle.model) == carModel && qs(vehicle.make) == flash_protocol_make_)
            {
                // A row's id is its catalog position.
                id.append(QString::number(i));
                version.append(qs(vehicle.version));
                type.append(qs(vehicle.type));
                kw.append(qs(vehicle.kw));
                hp.append(qs(vehicle.hp));
                fuel.append(qs(vehicle.fuel));
                year.append(qs(vehicle.year));
                ecu.append(protocolField(vehicle, &ProtocolSpec::ecu));
                mcu.append(protocolField(vehicle, &ProtocolSpec::mcu));
                mode.append(protocolField(vehicle, &ProtocolSpec::mode));
                checksum.append(checksumField(vehicle));
                read.append(protocolFlag(vehicle, &ProtocolSpec::read));
                write.append(protocolFlag(vehicle, &ProtocolSpec::write));
                family.append(qs(vehicle.protocol->name));
                description.append(protocolField(vehicle, &ProtocolSpec::description));
            }
        }

        qDebug() << "Add versions data items to select";
        const fastecu::Result<std::size_t> saved = config_.SelectedRow();
        for (int i = 0; i < version.length(); i++)
        {
            QTreeWidgetItem *itemLocal = new QTreeWidgetItem();

            itemLocal->setText(0, version.at(i));
            itemLocal->setText(1, type.at(i));
            itemLocal->setText(2, kw.at(i));
            itemLocal->setText(3, hp.at(i));
            itemLocal->setText(4, fuel.at(i));
            itemLocal->setText(5, year.at(i));
            itemLocal->setText(6, ecu.at(i));
            itemLocal->setText(7, mode.at(i));
            if (checksum.at(i) == "yes")
            {
                itemLocal->setCheckState(8, Qt::Checked);
                itemLocal->setForeground(8, Qt::darkGreen);
                itemLocal->setToolTip(8, "Checksum calculation supported");
            }
            else if (checksum.at(i) == "no")
            {
                itemLocal->setCheckState(8, Qt::Unchecked);
                itemLocal->setForeground(8, Qt::gray);
                itemLocal->setToolTip(8, "ROM has no checksum");
            }
            else if (checksum.at(i) == "n/a")
            {
                itemLocal->setCheckState(8, Qt::Checked);
                itemLocal->setForeground(8, Qt::red);
                itemLocal->setToolTip(8, "Checksum calculation NOT supported yet");
            }
            if (read.at(i) == "yes")
            {
                itemLocal->setCheckState(9, Qt::Checked);
            }
            else
            {
                itemLocal->setCheckState(9, Qt::Unchecked);
            }
            if (write.at(i) == "yes")
            {
                itemLocal->setCheckState(10, Qt::Checked);
            }
            else
            {
                itemLocal->setCheckState(10, Qt::Unchecked);
            }

            itemLocal->setText(11, family.at(i));
            itemLocal->setToolTip(11, family.at(i));
            itemLocal->setText(12, id.at(i));
            itemLocal->setText(13, description.at(i));
            // topLevelCarVersionTreeItem->setFirstColumnSpanned(true);
            ui_->car_version_tree_widget->addTopLevelItem(itemLocal);

            qDebug() << "Check if car version selected";
            if (saved.has_value() && id.at(i) == QString::number(*saved))
            {
                qDebug() << "Car version changed to saved model";
                carVersionChangedSaved = true;
                ui_->car_version_tree_widget->setCurrentItem(itemLocal);
            }
        }
        if (!carVersionChangedSaved)
        {
            qDebug() << "Car version changed to first item, no version selected previously";
            QTreeWidgetItem *itemLocal = ui_->car_version_tree_widget->topLevelItem(0);
            ui_->car_version_tree_widget->setCurrentItem(itemLocal);
        }
    }
    qDebug() << "Car model selection applied";
}

void VehicleSelect::carVersionTreewidgetItemSelected()
{
    QTreeWidgetItem *item = ui_->car_version_tree_widget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *itemLocal = ui_->car_version_tree_widget->selectedItems().at(0);
        QString selectedText = itemLocal->text(0);

        // qDebug() << "Selected version for model" << flash_protocol_model << "is" << selected_text;

        ui_->select_button->setEnabled(true);

        const QString& carVersion = selectedText;
        flash_protocol_version_.clear();
        flash_protocol_version_ = carVersion;
        flash_protocol_family_ = itemLocal->text(11);
        flash_protocol_id_ = itemLocal->text(12);
        flash_protocol_description_ = itemLocal->text(13);

        // flash_protocol_id = selected_text;//car_version_treewidget_item->text(11);

        // qDebug() << "Protocol ID:" << flash_protocol_id;
    }
    qDebug() << "Car version selection applied";
}
