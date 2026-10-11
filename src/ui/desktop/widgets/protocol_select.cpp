#include "src/ui/desktop/widgets/protocol_select.h"
#include "ui_protocol_select.h"

#include <algorithm>
#include <functional>

#include "src/ui/desktop/config_fields.h"

using fastecu::config::ProtocolSpec;
using fastecu::config::VehicleSpec;
using fastecu::ui::protocolField;
using fastecu::ui::qs;

ProtocolSelect::ProtocolSelect(const fastecu::config::ConfigSession& config, QWidget *parent)
    : QDialog(parent), config_(config), ui_{std::make_unique<Ui::ProtocolSelect>()}
{
    ui_->setupUi(this);

    // ui->select_button->setEnabled(false);

    QStringList treeWidgetHeaders = {"Model", "Protocol"};
    ui_->treeWidget->setHeaderLabels(treeWidgetHeaders);
    ui_->treeWidget->setFont(font_);

    // QRect  screenGeometry = this->geometry();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    // ui->treeWidget->setColumnWidth(0, 150);
    // ui->treeWidget->setColumnWidth(1, 75);

    QStringList protocols;
    QStringList protocolsSorted;
    QStringList descriptionsSorted;
    bool protocolChangedSaved = false;
    QFontMetrics fm(font_);
    int textWidth = 0;
    int protocolWidth = 0;
    int descriptionWidth = 0;

    // Vehicle-backed: one entry per distinct protocol any vehicle uses.
    const auto vehicles = config.Vehicles();
    for (const VehicleSpec& vehicle : vehicles)
    {
        if (!protocols.contains(qs(vehicle.protocol->name)))
        {
            protocols.append(qs(vehicle.protocol->name));
        }
    }

    protocolsSorted = protocols;
    std::sort(protocolsSorted.begin(), protocolsSorted.end(), std::less<QString>());

    for (int i = 0; i < protocolsSorted.length(); i++)
    {
        for (const VehicleSpec& vehicle : vehicles)
        {
            if (protocolsSorted.at(i) == qs(vehicle.protocol->name))
            {
                descriptionsSorted.append(protocolField(vehicle, &ProtocolSpec::description));
                textWidth = fm.horizontalAdvance(descriptionsSorted.at(i));
                if (textWidth > descriptionWidth)
                {
                    descriptionWidth = textWidth;
                }

                break;
            }
        }

        textWidth = fm.horizontalAdvance(protocolsSorted.at(i));
        if (textWidth > protocolWidth)
        {
            protocolWidth = textWidth;
        }
    }
    protocolWidth += 20;
    descriptionWidth += 20;
    ui_->treeWidget->setColumnWidth(0, protocolWidth);
    ui_->treeWidget->setColumnWidth(1, descriptionWidth);
    /*
        QScreen *screen = QGuiApplication::primaryScreen();
        QRect  screenGeometry = screen->geometry();
        if (this->width() < screenGeometry.width() && this->height() < screenGeometry.height())
            this->showMaximized();
    */
    ui_->treeWidget->setFixedWidth(protocolWidth + descriptionWidth + 20);

    for (int i = 0; i < protocolsSorted.length(); i++)
    {
        QTreeWidgetItem *item = new QTreeWidgetItem();
        item->setText(0, protocolsSorted.at(i));
        item->setText(1, descriptionsSorted.at(i));
        item->setFirstColumnSpanned(true);
        ui_->treeWidget->addTopLevelItem(item);
        if (config.SelectedVehicle() != nullptr &&
            protocolsSorted.at(i) == qs(config.SelectedVehicle()->protocol->name))
        {
            protocolChangedSaved = true;
            ui_->treeWidget->setCurrentItem(item);
        }
    }
    if (!protocolChangedSaved)
    {
        qDebug() << "Protocol changed to first item, no protocol selected previously";
        QTreeWidgetItem *item = ui_->treeWidget->topLevelItem(0);
        ui_->treeWidget->setCurrentItem(item);
    }

    connect(ui_->treeWidget, &QTreeWidget::itemSelectionChanged, this, &ProtocolSelect::protocolTreewidgetItemSelected);
    connect(ui_->treeWidget, &QTreeWidget::doubleClicked, this, &ProtocolSelect::carModelSelected);
    connect(ui_->cancel_button, &QPushButton::clicked, this, &QDialog::close);
    connect(ui_->select_button, &QPushButton::clicked, this, &ProtocolSelect::carModelSelected);
}

ProtocolSelect::~ProtocolSelect()
{
}

void ProtocolSelect::carModelSelected()
{
    QString protocolName = ui_->treeWidget->selectedItems().at(0)->text(0);
    qDebug() << "Selected protocol:" << protocolName;

    // Tentative: the caller applies an accepted choice to the session.
    chosenProtocolName = protocolName.toStdString();
    accept();

    close();
}

std::optional<std::string> ProtocolSelect::acceptedProtocolName() const
{
    return chosenProtocolName;
}

void ProtocolSelect::protocolTreewidgetItemSelected()
{
    QTreeWidgetItem *item = ui_->treeWidget->selectedItems().at(0);
    if (item)
    {
        QTreeWidgetItem *itemLocal = ui_->treeWidget->selectedItems().at(0);
        QString selectedText = itemLocal->text(0);

        // ui->select_button->setEnabled(true);
    }
    qDebug() << "protocol selection applied";
}
