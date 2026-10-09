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
    QFont font_;
    int font_size_ = 10;
    bool font_bold_ = false;
    QString font_family_ = "Franklin Gothic";
    int header_font_size_ = 10;
    bool header_font_bold_ = false;
    QString header_font_family_ = "Franklin Gothic";

    QString flash_protocol_id_;
    QString flash_protocol_mcu_;
    QString flash_protocol_make_;
    QString flash_protocol_model_;
    QString flash_protocol_version_;
    QString flash_protocol_type_;
    QString flash_protocol_kw_;
    QString flash_protocol_hp_;
    QString flash_protocol_fuel_;
    QString flash_protocol_year_;
    QString flash_protocol_ecu_;
    QString flash_protocol_mode_;
    QString flash_protocol_checksum_;
    QString flash_protocol_read_;
    QString flash_protocol_write_;
    QString flash_protocol_flash_protocol_;
    QString flash_protocol_log_protocol_;
    QString flash_protocol_comms_protocol_;
    QString flash_protocol_description_;
    QString flash_protocol_family_;

    const fastecu::config::ConfigSession& config_;
    std::optional<std::size_t> chosen_row_;

  private slots:
    void car_model_selected();
    void car_make_treewidget_item_selected();
    void car_model_treewidget_item_selected();
    void car_version_treewidget_item_selected();

  private:
    std::unique_ptr<Ui::VehicleSelect> ui_;
};
