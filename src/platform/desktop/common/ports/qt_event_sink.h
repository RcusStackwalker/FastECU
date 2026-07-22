#pragma once
#include <QObject>
#include <QString>
#include "src/backend/ports/event_sink.h"

// Marshals backend events to Qt signals for the GUI thread. Uses queued
// connections when connected across threads.
class QtEventSink : public QObject, public fastecu::IEventSink
{
    Q_OBJECT
  public:
    explicit QtEventSink(QObject *parent = nullptr) : QObject(parent)
    {
    }
    void log(fastecu::LogLevel, std::string_view message) override;
    void progress(int done, int total) override;
    void notice(std::string_view message) override;

  signals:
    void logged(int level, QString message);
    void progressed(int done, int total);
    void noticed(QString message);
};
