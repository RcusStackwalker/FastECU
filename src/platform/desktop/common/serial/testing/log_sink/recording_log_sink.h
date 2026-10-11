#pragma once

#include <QObject>
#include <QStringList>

class RecordingLogSink : public QObject
{
    Q_OBJECT

  public:
    QStringList messages;

  public slots:
    // NOLINTBEGIN(readability-identifier-naming): Qt slots keep Qt's camelBack names
    void logMessages(const QString& message, bool /*timestamp*/, bool /*linefeed*/)
    {
        messages << message;
    }
    // NOLINTEND(readability-identifier-naming)
};
