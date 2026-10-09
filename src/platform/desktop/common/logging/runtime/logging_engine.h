#pragma once

#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <cstdint>
#include <memory>
#include <optional>

#include "src/backend/logging/logging_protocol.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#include "src/platform/desktop/common/logging/runtime/logging_worker.h"
#include "src/platform/desktop/common/ports/event_sink/qt_event_sink.h"

namespace fastecu::desktop::logging
{

enum class LoggingStatus
{
    kRunning,
    kCarNotResponding,
};

enum class SessionEndReason
{
    kStoppedByUser,
    kHandshakeFailed,
    kAdapterDisconnected,
    kRuntimeFailed,
};

struct LogSessionConfig
{
    QString protocol_id;
};

using LoggingProtocolFactory =
    std::function<fastecu::Result<std::unique_ptr<fastecu::logging::LoggingProtocol>>(const DesktopLoggingSnapshot&)>;

class LoggingEngine final : public QObject
{
    Q_OBJECT
  public:
    explicit LoggingEngine(QObject *parent = nullptr);
    ~LoggingEngine() override;

    void RegisterProtocol(const QString& protocol_id, const LoggingProtocolFactory& factory);
    fastecu::Status Start(const LogSessionConfig& config, DesktopLoggingSnapshot snapshot);
    void Stop();
    bool IsRunning() const;

  signals:
    // NOLINTBEGIN(readability-identifier-naming): Qt signals keep Qt's camelBack names
    void valuesUpdated(QVector<fastecu::logging::LogSample> samples);
    void statusChanged(LoggingStatus status);
    void sessionEnded(SessionEndReason reason, QString message);
    void logE(QString message, bool timestamp, bool linefeed);
    void logW(QString message, bool timestamp, bool linefeed);
    void logI(QString message, bool timestamp, bool linefeed);
    void logD(QString message, bool timestamp, bool linefeed);
    // NOLINTEND(readability-identifier-naming): end of Qt block

  private slots:
    // NOLINTBEGIN(readability-identifier-naming): Qt slots keep Qt's camelBack names
    void handleWorkerStateChanged(fastecu::logging::LoggingState state);
    void handleWorkerSessionFinished(fastecu::Status result);
    void handleDiagnostic(int level, QString message);
    // NOLINTEND(readability-identifier-naming): end of Qt block

  private:
    void FinishActiveRun(SessionEndReason reason, QString detail, bool publish);
    void JoinAndReleaseActiveRun();
    void PublishCompletionOnce(SessionEndReason reason, QString detail);
    void ReportStartError(const fastecu::Error& error);

    // Private, but desktop_logging_protocol_registration_test.cpp and
    // desktop_composition_test.cpp compile this header under `#define private
    // public`, where clang-tidy would see these as public members.
    // NOLINTBEGIN(readability-identifier-naming)
    QMap<QString, LoggingProtocolFactory> registrations_;
    std::optional<DesktopLoggingSnapshot> active_snapshot_;
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
    // NOLINTEND(readability-identifier-naming)
};

} // namespace fastecu::desktop::logging

Q_DECLARE_METATYPE(fastecu::desktop::logging::LoggingStatus)
Q_DECLARE_METATYPE(fastecu::desktop::logging::SessionEndReason)
