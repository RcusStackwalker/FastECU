#ifndef PROTOCOL_SELECT_H
#define PROTOCOL_SELECT_H

#include <memory>

#include <QWidget>
#include <QStringListModel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QCheckBox>
#include <QScreen>

#include "src/backend/definitions/file_actions.h"

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
    explicit ProtocolSelect(FileActions::ConfigValuesStructure *configValues, QWidget *parent = nullptr);
    ~ProtocolSelect();

  private:
    QFont font;
    int font_size = 10;
    bool font_bold = false;
    QString font_family = "Franklin Gothic";
    int header_font_size = 10;
    bool header_font_bold = false;
    QString header_font_family = "Franklin Gothic";

    FileActions::ConfigValuesStructure *configValues;

  private slots:
    void car_model_selected();
    void protocol_treewidget_item_selected();

  private:
    std::unique_ptr<Ui::ProtocolSelect> ui;
};

#endif // PROTOCOL_SELECT_H
