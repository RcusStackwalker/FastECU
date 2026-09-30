#pragma once

#include <QObject>
#include <QStringList>

class RecordingLogSink : public QObject
{
    Q_OBJECT

  public:
    QStringList messages;

  public slots:
    void log_messages(const QString& message, bool /*timestamp*/, bool /*linefeed*/)
    {
        messages << message;
    }
};
