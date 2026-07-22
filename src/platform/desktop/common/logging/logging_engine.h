#pragma once

#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

#include "src/backend/logging/logging_protocol.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#include "src/platform/desktop/common/logging/logging_worker.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"

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
};

struct LogSessionConfig
{
    QString protocolId;
};

using LoggingProtocolFactory = std::function<std::unique_ptr<fastecu::logging::LoggingProtocol>(
    const fastecu::desktop::logging::DesktopLoggingSnapshot&)>;

class LoggingEngine final : public QObject
{
    Q_OBJECT
  public:
    explicit LoggingEngine(QObject *parent = nullptr);
    ~LoggingEngine() override;

    void registerProtocol(const QString& protocol_id, LoggingProtocolFactory factory);
    bool start(LogSessionConfig config,
               fastecu::desktop::logging::DesktopLoggingSnapshot snapshot);
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
    void clearActiveSession();

    QMap<QString, LoggingProtocolFactory> registrations_;
    std::optional<fastecu::desktop::logging::DesktopLoggingSnapshot> active_snapshot_;
    std::unique_ptr<fastecu::logging::LoggingProtocol> active_protocol_;
    LoggingWorker *active_worker_ = nullptr;
    QtEventSink diagnostics_;
    std::optional<LoggingStatus> last_status_;
};

Q_DECLARE_METATYPE(LoggingStatus)
Q_DECLARE_METATYPE(SessionEndReason)
