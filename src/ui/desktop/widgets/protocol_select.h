#pragma once

#include <memory>
#include <optional>
#include <string>

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
class ProtocolSelect;
}
QT_END_NAMESPACE

class ProtocolSelect : public QDialog
{
    Q_OBJECT

  public:
    explicit ProtocolSelect(const fastecu::config::ConfigSession& config, QWidget *parent = nullptr);
    ~ProtocolSelect();

    // The accepted protocol name; empty until the operator chooses one. The
    // session itself is never changed here: the caller applies an accepted
    // choice.
    std::optional<std::string> chosen_protocol_name() const;

  private:
    QFont font;
    int font_size = 10;
    bool font_bold = false;
    QString font_family = "Franklin Gothic";
    int header_font_size = 10;
    bool header_font_bold = false;
    QString header_font_family = "Franklin Gothic";

    const fastecu::config::ConfigSession& config;
    std::optional<std::string> chosenProtocolName;

  private slots:
    void car_model_selected();
    void protocol_treewidget_item_selected();

  private:
    std::unique_ptr<Ui::ProtocolSelect> ui;
};
