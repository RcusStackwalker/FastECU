#pragma once

#include <QWidget>
#include <QGroupBox>
#include <QLabel>
#include <QGuiApplication>
#include <QScreen>
#include <QRect>
#include <QBoxLayout>
#include <QDebug>

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

  private:
  signals:
};
