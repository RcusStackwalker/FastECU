#pragma once

#include <memory>

#include <QFileDialog>
#include <QMessageBox>
#include <QDialog>

QT_BEGIN_NAMESPACE
namespace Ui
{
class EcuOperationsWindow;
}
QT_END_NAMESPACE

class GetKeyOperationsSubaru : public QDialog
{
    Q_OBJECT

  signals:
    void logE(QString message, bool timestamp, bool linefeed);
    void logW(QString message, bool timestamp, bool linefeed);
    void logI(QString message, bool timestamp, bool linefeed);
    void logD(QString message, bool timestamp, bool linefeed);

  public:
    explicit GetKeyOperationsSubaru(QWidget *parent = nullptr);
    ~GetKeyOperationsSubaru();

  private:
    void closeEvent(QCloseEvent *bar);

    bool kill_process_ = false;

    int loadAndApplyLinearApprox();

  private:
    std::unique_ptr<Ui::EcuOperationsWindow> ui_;
};
