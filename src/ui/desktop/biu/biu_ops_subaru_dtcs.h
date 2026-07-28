#pragma once

#include <memory>

#include <QWidget>
#include <QLabel>
#include <QDebug>

QT_BEGIN_NAMESPACE
namespace Ui
{
class BiuOpsSubaruDtcsWindow;
}
QT_END_NAMESPACE

class BiuOpsSubaruDtcs : public QWidget
{
    Q_OBJECT

  public:
    explicit BiuOpsSubaruDtcs(QStringList *dtc_result, QWidget *parent = nullptr);
    ~BiuOpsSubaruDtcs();

  private:
    QStringList *dtc_result;

    void closeEvent(QCloseEvent *event);

  private:
    std::unique_ptr<Ui::BiuOpsSubaruDtcsWindow> ui;
};
