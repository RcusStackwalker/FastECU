#pragma once

#include <cstddef>
#include <memory>
#include <optional>

// #include <QDesktopWidget>
#include <QWidget>
#include <QDialog>
#include <QStringListModel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QCheckBox>
#include <QScreen>

#include "src/backend/config/config_session.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class VehicleSelect;
}
QT_END_NAMESPACE

class VehicleSelect : public QDialog
{
    Q_OBJECT

  public:
    explicit VehicleSelect(const fastecu::config::ConfigSession& config, QWidget *parent = nullptr);
    ~VehicleSelect();

    // The accepted row; empty until the operator chooses one. The session
    // itself is never changed here: the caller applies an accepted choice.
    std::optional<std::size_t> chosen_row() const;

  private:
    QFont font;
    int font_size = 10;
    bool font_bold = false;
    QString font_family = "Franklin Gothic";
    int header_font_size = 10;
    bool header_font_bold = false;
    QString header_font_family = "Franklin Gothic";

    QString flash_protocol_id;
    QString flash_protocol_mcu;
    QString flash_protocol_make;
    QString flash_protocol_model;
    QString flash_protocol_version;
    QString flash_protocol_type;
    QString flash_protocol_kw;
    QString flash_protocol_hp;
    QString flash_protocol_fuel;
    QString flash_protocol_year;
    QString flash_protocol_ecu;
    QString flash_protocol_mode;
    QString flash_protocol_checksum;
    QString flash_protocol_read;
    QString flash_protocol_write;
    QString flash_protocol_flash_protocol;
    QString flash_protocol_log_protocol;
    QString flash_protocol_comms_protocol;
    QString flash_protocol_description;
    QString flash_protocol_family;

    const fastecu::config::ConfigSession& config;
    std::optional<std::size_t> chosenRow;

  private slots:
    void car_model_selected();
    void car_make_treewidget_item_selected();
    void car_model_treewidget_item_selected();
    void car_version_treewidget_item_selected();

  private:
    std::unique_ptr<Ui::VehicleSelect> ui;
};
