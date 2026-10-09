#pragma once

#include <memory>

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QMainWindow>
#include <QSerialPort>
#include <QTime>
#include <QTimer>
#include <QWidget>
#include <QDialog>
#include <QTableWidget>
#include <QGroupBox>
#include <QLabel>

QT_BEGIN_NAMESPACE
namespace Ui
{
class BiuOpsSubaruSwitchesWindow;
}
QT_END_NAMESPACE

class BiuOpsSubaruSwitches : public QWidget
{
    Q_OBJECT

  public:
    explicit BiuOpsSubaruSwitches(QStringList *switchResult, QWidget *parent = nullptr);
    ~BiuOpsSubaruSwitches();

    void update_switch_results(QStringList *switchResult);

  private:
    // QGroupBox *gridGroupBox;
    // QLabel *label;

    QStringList *switch_result_;

  private slots:

  signals:

  private:
    std::unique_ptr<Ui::BiuOpsSubaruSwitchesWindow> ui_;
};
