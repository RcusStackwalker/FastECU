#pragma once

#include <memory>

#include <QWidget>
#include <QLabel>

QT_BEGIN_NAMESPACE
namespace Ui
{
class BiuOpsSubaruDataWindow;
}
QT_END_NAMESPACE

class BiuOpsSubaruData : public QWidget
{
    Q_OBJECT

  public:
    explicit BiuOpsSubaruData(QStringList *dataResult, QWidget *parent = nullptr);
    ~BiuOpsSubaruData();

    void updateDataResults(QStringList *dataResult);

  private:
    QStringList *data_result_;

  private:
    std::unique_ptr<Ui::BiuOpsSubaruDataWindow> ui_;
};
