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
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  public:
    explicit GetKeyOperationsSubaru(QWidget *parent = nullptr);
    ~GetKeyOperationsSubaru();

  private:
    void closeEvent(QCloseEvent *bar);

    bool kill_process = false;

    int load_and_apply_linear_approx();

  private:
    std::unique_ptr<Ui::EcuOperationsWindow> ui;
};
