#include "src/ui/desktop/widgets/vehicle_select.h"
#include "ui_vehicle_select.h"

#include <algorithm>
#include <functional>

#include "src/ui/desktop/config_fields.h"

using fastecu::config::ProtocolSpec;
using fastecu::config::VehicleSpec;
using fastecu::ui::checksum_field;
using fastecu::ui::protocol_field;
using fastecu::ui::protocol_flag;
using fastecu::ui::qs;

VehicleSelect::VehicleSelect(const fastecu::config::ConfigSession& config, QWidget *parent)
    : QDialog(parent), config_(config), ui_{std::make_unique<Ui::VehicleSelect>()}
{
    ui_->setupUi(this);

    ui_->select_button->setEnabled(false);

    ui_->car_make_tree_widget->setHeaderLabel("Manufacturer");
    ui_->car_model_tree_widget->setHeaderLabel("Model");
    QStringList car_version_tree_widget_headers = {"Version", "Type", "kW",  "HP", "Fuel", "Year",
                                                   "ECU",     "Mode", "CHK", "RD", "WR",   "Protocol"};
    ui_->car_version_tree_widget->setHeaderLabels(car_version_tree_widget_headers);
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

    const VehicleSpec *selected = config.selected_vehicle();
    const QString selected_make = selected != nullptr ? qs(selected->make) : QString();

    QStringList car_makes;
    QStringList car_makes_sorted;
    bool car_make_changed_saved = false;

    for (const VehicleSpec& vehicle : config.vehicles())
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
        item->setFont(0, font_);
        item->setText(0, car_makes_sorted.at(i));
        item->setFirstColumnSpanned(true);
        ui_->car_make_tree_widget->addTopLevelItem(item);
        if (car_makes_sorted.at(i) == selected_make)
        {
            car_make_changed_saved = true;
            ui_->car_make_tree_widget->setCurrentItem(item);
        }
    }
    if (!car_make_changed_saved)
    {
        qDebug() << "Car make changed to first item, no make selected previously";
        QTreeWidgetItem *item = ui_->car_make_tree_widget->topLevelItem(0);
        ui_->car_make_tree_widget->setCurrentItem(item);
    }

    connect(ui_->car_make_tree_widget, &QTreeWidget::itemSelectionChanged, this,
            &VehicleSelect::car_make_treewidget_item_selected);
    connect(ui_->car_model_tree_widget, &QTreeWidget::itemSelectionChanged, this,
            &VehicleSelect::car_model_treewidget_item_selected);
    connect(ui_->car_version_tree_widget, &QTreeWidget::itemSelectionChanged, this,
            &VehicleSelect::car_version_treewidget_item_selected);
    connect(ui_->car_version_tree_widget, &QTreeWidget::itemDoubleClicked, this, &VehicleSelect::car_model_selected);
    connect(ui_->cancel_button, &QPushButton::clicked, this, &QDialog::close);
    connect(ui_->select_button, &QPushButton::clicked, this, &VehicleSelect::car_model_selected);

    const QModelIndex make_index = ui_->car_make_tree_widget->selectionModel()->currentIndex();
    ui_->car_make_tree_widget->setCurrentIndex(make_index);
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

void VehicleSelect::car_model_selected()
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

std::optional<std::size_t> VehicleSelect::chosen_row() const
{
    return chosen_row_;
}

void VehicleSelect::car_make_treewidget_item_selected()
{
    qDebug() << "Car make selection changed";
    QTreeWidgetItem *item = ui_->car_make_tree_widget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *item_local = ui_->car_make_tree_widget->selectedItems().at(0);
        QString selected_text = item_local->text(0);

        // qDebug() << "Check models for manufacturer:" << selected_text;

        const QString& car_make = selected_text;
        flash_protocol_id_.clear();
        flash_protocol_make_ = car_make;
        flash_protocol_model_.clear();
        flash_protocol_version_.clear();

        QStringList car_models;
        QStringList car_models_sorted;
        bool car_model_changed_saved = false;

        // To delete treewidget items, disconnect itemSelectionChanged() signal first
        disconnect(ui_->car_model_tree_widget, &QTreeWidget::itemSelectionChanged, 0, 0);
        qDebug() << "Delete car_model_tree_widget items";
        int item_count = ui_->car_model_tree_widget->topLevelItemCount();
        for (int i = 0; i < item_count; i++)
        {
            QTreeWidgetItem *item_local = ui_->car_model_tree_widget->topLevelItem(0);
            delete item_local;
        }
        // Connect itemSelectionChanged() signal again
        connect(ui_->car_model_tree_widget, &QTreeWidget::itemSelectionChanged, this,
                &VehicleSelect::car_model_treewidget_item_selected);

        qDebug() << "Add models data based on selected make";
        for (const VehicleSpec& vehicle : config_.vehicles())
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
            ui_->car_model_tree_widget->addTopLevelItem(item_local);
            if (config_.selected_vehicle() != nullptr &&
                car_models_sorted.at(i) == qs(config_.selected_vehicle()->model))
            {
                qDebug() << "Car model changed to saved model";
                car_model_changed_saved = true;
                ui_->car_model_tree_widget->setCurrentItem(item_local);
            }
        }
        if (!car_model_changed_saved)
        {
            qDebug() << "Car model changed to first item, no model selected previously";
            QTreeWidgetItem *item_local = ui_->car_model_tree_widget->topLevelItem(0);
            ui_->car_model_tree_widget->setCurrentItem(item_local);
        }
    }
    qDebug() << "Car make selection applied";
}

void VehicleSelect::car_model_treewidget_item_selected()
{
    qDebug() << "Car model selection changed";
    QTreeWidgetItem *item = ui_->car_model_tree_widget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *item_local = ui_->car_model_tree_widget->selectedItems().at(0);
        QString selected_text = item_local->text(0);

        // qDebug() << "Check versions for model:" << selected_text;

        const QString& car_model = selected_text;
        QStringList car_versions;
        QStringList car_versions_sorted;
        bool car_version_changed_saved = false;

        flash_protocol_id_.clear();
        flash_protocol_model_ = car_model;
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
        int item_count = ui_->car_version_tree_widget->topLevelItemCount();
        for (int i = 0; i < item_count; i++)
        {
            QTreeWidgetItem *item_local = ui_->car_version_tree_widget->topLevelItem(0);
            delete item_local;
        }
        // Connect itemSelectionChanged() signal again
        connect(ui_->car_version_tree_widget, &QTreeWidget::itemSelectionChanged, this,
                &VehicleSelect::car_version_treewidget_item_selected);

        qDebug() << "Add versions data based on selected model";
        const auto vehicles = config_.vehicles();
        for (std::size_t i = 0; i < vehicles.size(); i++)
        {
            const VehicleSpec& vehicle = vehicles[i];
            if (qs(vehicle.model) == car_model && qs(vehicle.make) == flash_protocol_make_)
            {
                // A row's id is its catalog position.
                id.append(QString::number(i));
                version.append(qs(vehicle.version));
                type.append(qs(vehicle.type));
                kw.append(qs(vehicle.kw));
                hp.append(qs(vehicle.hp));
                fuel.append(qs(vehicle.fuel));
                year.append(qs(vehicle.year));
                ecu.append(protocol_field(vehicle, &ProtocolSpec::ecu));
                mcu.append(protocol_field(vehicle, &ProtocolSpec::mcu));
                mode.append(protocol_field(vehicle, &ProtocolSpec::mode));
                checksum.append(checksum_field(vehicle));
                read.append(protocol_flag(vehicle, &ProtocolSpec::read));
                write.append(protocol_flag(vehicle, &ProtocolSpec::write));
                family.append(qs(vehicle.protocol->name));
                description.append(protocol_field(vehicle, &ProtocolSpec::description));
            }
        }

        qDebug() << "Add versions data items to select";
        const fastecu::Result<std::size_t> saved = config_.selected_row();
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
            ui_->car_version_tree_widget->addTopLevelItem(item_local);

            qDebug() << "Check if car version selected";
            if (saved.has_value() && id.at(i) == QString::number(*saved))
            {
                qDebug() << "Car version changed to saved model";
                car_version_changed_saved = true;
                ui_->car_version_tree_widget->setCurrentItem(item_local);
            }
        }
        if (!car_version_changed_saved)
        {
            qDebug() << "Car version changed to first item, no version selected previously";
            QTreeWidgetItem *item_local = ui_->car_version_tree_widget->topLevelItem(0);
            ui_->car_version_tree_widget->setCurrentItem(item_local);
        }
    }
    qDebug() << "Car model selection applied";
}

void VehicleSelect::car_version_treewidget_item_selected()
{
    QTreeWidgetItem *item = ui_->car_version_tree_widget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *item_local = ui_->car_version_tree_widget->selectedItems().at(0);
        QString selected_text = item_local->text(0);

        // qDebug() << "Selected version for model" << flash_protocol_model << "is" << selected_text;

        ui_->select_button->setEnabled(true);

        const QString& car_version = selected_text;
        flash_protocol_version_.clear();
        flash_protocol_version_ = car_version;
        flash_protocol_family_ = item_local->text(11);
        flash_protocol_id_ = item_local->text(12);
        flash_protocol_description_ = item_local->text(13);

        // flash_protocol_id = selected_text;//car_version_treewidget_item->text(11);

        // qDebug() << "Protocol ID:" << flash_protocol_id;
    }
    qDebug() << "Car version selection applied";
}
