#include "protocol_select.h"
#include "ui_protocol_select.h"

#include <algorithm>
#include <functional>

#include "src/ui/desktop/config_fields.h"

using fastecu::config::ProtocolEntry;
using fastecu::config::ResolvedCarModel;
using fastecu::ui::protocol_field;
using fastecu::ui::qs;

ProtocolSelect::ProtocolSelect(const fastecu::config::ConfigSession& config, QWidget *parent)
    : QDialog(parent), config(config), ui{std::make_unique<Ui::ProtocolSelect>()}
{
    ui->setupUi(this);

    // ui->select_button->setEnabled(false);

    QStringList tree_widget_headers = {"Model", "Protocol"};
    ui->treeWidget->setHeaderLabels(tree_widget_headers);
    ui->treeWidget->setFont(font);

    // QRect  screenGeometry = this->geometry();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    // ui->treeWidget->setColumnWidth(0, 150);
    // ui->treeWidget->setColumnWidth(1, 75);

    QStringList protocols;
    QStringList protocols_sorted;
    QStringList descriptions_sorted;
    bool protocol_changed_saved = false;
    QFontMetrics fm(font);
    int text_width = 0;
    int protocol_width = 0;
    int description_width = 0;

    // Vehicle-backed: one entry per distinct protocol name any vehicle uses,
    // unresolved references included.
    const auto vehicles = config.vehicles();
    for (const ResolvedCarModel& vehicle : vehicles)
    {
        if (!protocols.contains(qs(vehicle.protocol_name)))
        {
            protocols.append(qs(vehicle.protocol_name));
        }
    }

    protocols_sorted = protocols;
    std::sort(protocols_sorted.begin(), protocols_sorted.end(), std::less<QString>());

    for (int i = 0; i < protocols_sorted.length(); i++)
    {
        for (const ResolvedCarModel& vehicle : vehicles)
        {
            if (protocols_sorted.at(i) == qs(vehicle.protocol_name))
            {
                descriptions_sorted.append(protocol_field(vehicle, &ProtocolEntry::description));
                text_width = fm.horizontalAdvance(descriptions_sorted.at(i));
                if (text_width > description_width)
                {
                    description_width = text_width;
                }

                break;
            }
        }

        text_width = fm.horizontalAdvance(protocols_sorted.at(i));
        if (text_width > protocol_width)
        {
            protocol_width = text_width;
        }
    }
    protocol_width += 20;
    description_width += 20;
    ui->treeWidget->setColumnWidth(0, protocol_width);
    ui->treeWidget->setColumnWidth(1, description_width);
    /*
        QScreen *screen = QGuiApplication::primaryScreen();
        QRect  screenGeometry = screen->geometry();
        if (this->width() < screenGeometry.width() && this->height() < screenGeometry.height())
            this->showMaximized();
    */
    ui->treeWidget->setFixedWidth(protocol_width + description_width + 20);

    for (int i = 0; i < protocols_sorted.length(); i++)
    {
        QTreeWidgetItem *item = new QTreeWidgetItem();
        item->setText(0, protocols_sorted.at(i));
        item->setText(1, descriptions_sorted.at(i));
        item->setFirstColumnSpanned(true);
        ui->treeWidget->addTopLevelItem(item);
        if (config.selected_vehicle() != nullptr &&
            protocols_sorted.at(i) == qs(config.selected_vehicle()->protocol_name))
        {
            protocol_changed_saved = true;
            ui->treeWidget->setCurrentItem(item);
        }
    }
    if (!protocol_changed_saved)
    {
        qDebug() << "Protocol changed to first item, no protocol selected previously";
        QTreeWidgetItem *item = ui->treeWidget->topLevelItem(0);
        ui->treeWidget->setCurrentItem(item);
    }

    connect(ui->treeWidget, &QTreeWidget::itemSelectionChanged, this,
            &ProtocolSelect::protocol_treewidget_item_selected);
    connect(ui->treeWidget, &QTreeWidget::doubleClicked, this, &ProtocolSelect::car_model_selected);
    connect(ui->cancel_button, &QPushButton::clicked, this, &QDialog::close);
    connect(ui->select_button, &QPushButton::clicked, this, &ProtocolSelect::car_model_selected);
}

ProtocolSelect::~ProtocolSelect()
{
}

void ProtocolSelect::car_model_selected()
{
    QString protocol_name = ui->treeWidget->selectedItems().at(0)->text(0);
    qDebug() << "Selected protocol:" << protocol_name;

    // Tentative: the caller applies an accepted choice to the session.
    chosenProtocolName = protocol_name.toStdString();
    accept();

    close();
}

std::optional<std::string> ProtocolSelect::chosen_protocol_name() const
{
    return chosenProtocolName;
}

void ProtocolSelect::protocol_treewidget_item_selected()
{
    QTreeWidgetItem *item = ui->treeWidget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *item_local = ui->treeWidget->selectedItems().at(0);
        QString selected_text = item_local->text(0);

        // ui->select_button->setEnabled(true);
    }
    qDebug() << "protocol selection applied";
}
