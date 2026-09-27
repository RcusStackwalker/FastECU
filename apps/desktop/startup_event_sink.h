#pragma once
#include <string_view>

#include <QString>
#include <QStringList>

#include "src/backend/ports/event_sink.h"

// Collects what the configuration session reports while nothing that could
// display it (window, syslog thread) exists yet. Warnings, errors, and
// notices are kept for the startup presenter; debug/info lines are dropped.
class StartupEventSink : public fastecu::IEventSink
{
  public:
    void log(fastecu::LogLevel level, std::string_view message) override
    {
        if (level == fastecu::LogLevel::Warning || level == fastecu::LogLevel::Error)
        {
            warnings_.append(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
        }
    }
    void progress(int, int) override
    {
    }
    void notice(std::string_view message) override
    {
        warnings_.append(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
    }
    const QStringList& warnings() const
    {
        return warnings_;
    }

  private:
    QStringList warnings_;
};
