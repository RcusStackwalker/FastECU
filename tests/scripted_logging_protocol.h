#pragma once
#include "src/backend/logging/logging_protocol.h"
#include "src/backend/ports/result.h"
#include <QList>

// Test double: scripts a sequence of start()/poll() outcomes for LoggingWorker
// and LoggingEngine tests. Once a queue is exhausted, start() fails with a
// diagnostic message and poll() returns a responded=true PollData with no
// samples (a safe default that never spuriously degrades a test's status
// assertions).
class ScriptedLoggingProtocol : public LoggingProtocol
{
  public:
    void queueStartResult(bool ok, const QString& error = QString())
    {
        startResultsOk_.append(ok);
        startResultsError_.append(error);
    }
    // Scripted "Ok" step: pass responded=true with samples.
    // Scripted "NoResponse" step: pass responded=false (samples ignored).
    void queuePollResult(bool responded, const QVector<LogSample>& samples = {})
    {
        pollIsError_.append(false);
        pollResponded_.append(responded);
        pollSamples_.append(samples);
        pollErrorMessage_.append(QString());
    }
    // Scripted "TransportError" step.
    void queuePollError(const QString& errorMessage)
    {
        pollIsError_.append(true);
        pollResponded_.append(false);
        pollSamples_.append(QVector<LogSample>());
        pollErrorMessage_.append(errorMessage);
    }

    int startCallCount() const
    {
        return startCalls_;
    }
    bool stopCalled() const
    {
        return stopCalled_;
    }

    fastecu::Status start() override
    {
        ++startCalls_;
        if (startIdx_ >= startResultsOk_.size())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "no scripted start result");
        }
        bool ok = startResultsOk_.at(startIdx_);
        QString error = startResultsError_.at(startIdx_);
        ++startIdx_;
        if (!ok)
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, error.toStdString());
        }
        return {};
    }

    fastecu::Result<PollData> poll(int) override
    {
        if (pollIdx_ >= pollIsError_.size())
        {
            return PollData{true, {}};
        }
        int i = pollIdx_++;
        if (pollIsError_.at(i))
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, pollErrorMessage_.at(i).toStdString());
        }
        return PollData{pollResponded_.at(i), pollSamples_.at(i)};
    }

    void stop() override
    {
        stopCalled_ = true;
    }

  private:
    QList<bool> startResultsOk_;
    QList<QString> startResultsError_;
    QList<bool> pollIsError_;
    QList<bool> pollResponded_;
    QList<QVector<LogSample>> pollSamples_;
    QList<QString> pollErrorMessage_;
    int startIdx_ = 0;
    int pollIdx_ = 0;
    int startCalls_ = 0;
    bool stopCalled_ = false;
};
