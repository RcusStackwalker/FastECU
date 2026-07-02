#pragma once
#include <QMap>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include "logging/logging_protocol.h"

class LoggingWorker;

struct LogSessionConfig {
    QString protocolId;              // registry key: "SSM" / "MUT_DMA" / "CDBG"
    QString logValueProtocolFilter;  // string compared against log_value_protocol entries
};

using LoggingProtocolFactory = std::function<std::unique_ptr<LoggingProtocol>(const LogSessionConfig &)>;

// GUI-thread-owned façade over the current logging session's LoggingWorker.
// Adding a future protocol means one call to registerProtocol() -- no changes
// to LoggingEngine or LoggingWorker themselves.
class LoggingEngine : public QObject {
    Q_OBJECT
public:
    explicit LoggingEngine(QObject *parent = nullptr);
    ~LoggingEngine() override;

    void registerProtocol(const QString &protocolId, LoggingProtocolFactory factory,
                           int pollTimeoutMs, int carSilenceMissThreshold,
                           int reconnectAttemptThreshold, int reconnectRetryPeriod);

    // Returns false immediately without starting anything if no factory is
    // registered for config.protocolId, or a session is already running.
    bool start(const LogSessionConfig &config);
    void stop();
    bool isRunning() const;

signals:
    void valuesUpdated(QVector<LogSample> samples);
    void statusChanged(LoggingStatus status);
    void sessionEnded(SessionEndReason reason, QString message);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

private slots:
    void handleWorkerSessionEnded(SessionEndReason reason, QString message);

private:
    struct ProtocolRegistration {
        LoggingProtocolFactory factory;
        int pollTimeoutMs;
        int carSilenceMissThreshold;
        int reconnectAttemptThreshold;
        int reconnectRetryPeriod;
    };

    QMap<QString, ProtocolRegistration> m_registrations;
    std::unique_ptr<LoggingProtocol> m_activeProtocol;
    LoggingWorker *m_activeWorker = nullptr;
};
