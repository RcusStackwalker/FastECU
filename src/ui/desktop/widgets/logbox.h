#pragma once

#include <QWidget>
#include <QGroupBox>
#include <QLabel>
#include <QGuiApplication>
#include <QScreen>
#include <QRect>
#include <QBoxLayout>
#include <QDebug>

QT_BEGIN_NAMESPACE
namespace Ui
{
class Settings;
}
QT_END_NAMESPACE

class LogBox : public QWidget
{
    Q_OBJECT
  public:
    explicit LogBox(QWidget *parent = nullptr);

    QGroupBox *drawLogBoxes(const QString& type, int index, int switchBoxCount, const QString& title,
                            const QString& unit, const QString& value);
    QGroupBox *drawLogSwitchBox(int index, int switchBoxCount, const QString& title, const QString& unit,
                                const QString& value);
    QGroupBox *drawLogValueBox(int index, int logBoxCount, const QString& title, const QString& unit,
                               const QString& value);
    void updateSwitchBox();
    void updateLogBox();

  private:
  signals:
};
