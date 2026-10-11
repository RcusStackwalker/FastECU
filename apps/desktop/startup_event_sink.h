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
    void Log(fastecu::LogLevel level, std::string_view message) override
    {
        if (level == fastecu::LogLevel::kWarning || level == fastecu::LogLevel::kError)
        {
            warnings_.append(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
        }
    }
    void Progress(int, int) override
    {
        // Configuration startup has no progress consumer; this sink collects diagnostics for the presenter.
    }
    void Notice(std::string_view message) override
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
