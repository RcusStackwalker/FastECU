#pragma once
#include <QAtomicInteger>
#include <QThread>
#include "src/backend/logging/logging_protocol.h"

// Drives one LoggingProtocol in a dedicated thread. Does not use a QTimer or the
// thread's own event loop for pacing: run() is a tight loop where each protocol's
// own blocking poll() call provides the pacing, so a polling-type protocol sends
// its next request immediately upon receiving the previous response, and a
// setup-and-listen protocol processes each frame as soon as it arrives.
//
// requestStop() is safe to call from any thread without the worker running an
// event loop, because it only sets an atomic flag that run()'s loop checks between
// poll() calls (each bounded by pollTimeoutMs).
class LoggingWorker : public QThread
{
    Q_OBJECT
  public:
    LoggingWorker(LoggingProtocol *protocol, int pollTimeoutMs,
                  int carSilenceMissThreshold, int reconnectAttemptThreshold,
                  int reconnectRetryPeriod, QObject *parent = nullptr);
    ~LoggingWorker() override;

    void requestStop();

  signals:
    void valuesUpdated(QVector<LogSample> samples);
    void statusChanged(LoggingStatus status);
    void sessionEnded(SessionEndReason reason, QString message);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  protected:
    void run() override;

  private:
    LoggingProtocol *m_protocol; // not owned
    int m_pollTimeoutMs;
    int m_carSilenceMissThreshold;
    int m_reconnectAttemptThreshold;
    int m_reconnectRetryPeriod;
    QAtomicInteger<bool> m_stopRequested{false};
};
