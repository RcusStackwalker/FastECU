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
    void Log(fastecu::LogLevel, std::string_view message) override;
    void Progress(int done, int total) override;
    void PhaseProgress(const fastecu::PhaseProgressEvent& event) override;
    void Notice(std::string_view message) override;

  signals:
    // NOLINTBEGIN(readability-identifier-naming): Qt signals keep Qt's camelBack names
    void logged(int level, QString message);
    void progressed(int done, int total);
    void phaseProgressed(QString phase_name, int phase_index, int phase_count, int done, int total);
    void noticed(QString message);
    // NOLINTEND(readability-identifier-naming)
};
