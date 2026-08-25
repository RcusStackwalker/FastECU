#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>
#include <optional>

#include "src/backend/logging/logging_protocol.h"
#include "src/backend/logging/logging_session.h"
#include "src/platform/desktop/common/logging/logging_worker.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"

namespace fastecu::desktop::logging
{

enum class LoggingStatus
{
    Running,
    CarNotResponding,
};

enum class SessionEndReason
{
    StoppedByUser,
    HandshakeFailed,
    AdapterDisconnected,
    RuntimeFailed,
};

struct LoggingRun
{
    fastecu::logging::LoggingSession session;
    std::unique_ptr<fastecu::logging::LoggingProtocol> protocol;
};

class LoggingEngine final : public QObject
{
    Q_OBJECT
  public:
    explicit LoggingEngine(QObject *parent = nullptr);
    ~LoggingEngine() override;

    fastecu::Status start(LoggingRun run);
    void stop();
    bool isRunning() const;

  signals:
    void valuesUpdated(QVector<fastecu::logging::LogSample> samples);
    void statusChanged(LoggingStatus status);
    void sessionEnded(SessionEndReason reason, QString message);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  private slots:
    void handleWorkerStateChanged(fastecu::logging::LoggingState state);
    void handleWorkerSessionFinished(fastecu::Status result);
    void handleDiagnostic(int level, QString message);

  private:
    void finishActiveRun(SessionEndReason reason, QString detail, bool publish);
    void joinAndReleaseActiveRun();
    void publishCompletionOnce(SessionEndReason reason, QString detail);
    void reportStartError(const fastecu::Error& error);

    std::optional<fastecu::logging::LoggingSession> active_session_;
    std::unique_ptr<fastecu::logging::LoggingProtocol> active_protocol_;
    LoggingWorker *active_worker_ = nullptr;
    QtEventSink diagnostics_;
    std::optional<LoggingStatus> last_status_;
    std::uint64_t active_run_generation_ = 0;
    std::uint64_t next_run_generation_ = 0;
    bool worker_reached_running_ = false;
    bool explicit_stop_pending_ = false;
    bool destroying_ = false;
    bool completion_published_ = false;
};

} // namespace fastecu::desktop::logging

Q_DECLARE_METATYPE(fastecu::desktop::logging::LoggingStatus)
Q_DECLARE_METATYPE(fastecu::desktop::logging::SessionEndReason)
