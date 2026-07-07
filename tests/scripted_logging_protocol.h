#pragma once
#include "logging/logging_protocol.h"
#include <QList>

// Test double: scripts a sequence of start()/poll() outcomes for LoggingWorker
// and LoggingEngine tests. Once a queue is exhausted, start() fails with a
// diagnostic message and poll() returns Ok with no samples (a safe default that
// never spuriously degrades a test's status assertions).
class ScriptedLoggingProtocol : public LoggingProtocol
{
  public:
    void queueStartResult(bool ok, const QString& error = QString())
    {
        startResultsOk_.append(ok);
        startResultsError_.append(error);
    }
    void queuePollResult(const PollResult& result)
    {
        pollResults_.append(result);
    }

    int startCallCount() const
    {
        return startCalls_;
    }
    bool stopCalled() const
    {
        return stopCalled_;
    }

    bool start(QString *errorOut) override
    {
        ++startCalls_;
        if (startIdx_ >= startResultsOk_.size())
        {
            if (errorOut)
                *errorOut = "no scripted start result";
            return false;
        }
        bool ok = startResultsOk_.at(startIdx_);
        if (errorOut)
            *errorOut = startResultsError_.at(startIdx_);
        ++startIdx_;
        return ok;
    }

    PollResult poll(int) override
    {
        if (pollIdx_ >= pollResults_.size())
        {
            PollResult r;
            r.status = PollResult::Status::Ok;
            return r;
        }
        return pollResults_.at(pollIdx_++);
    }

    void stop() override
    {
        stopCalled_ = true;
    }

  private:
    QList<bool> startResultsOk_;
    QList<QString> startResultsError_;
    QList<PollResult> pollResults_;
    int startIdx_ = 0;
    int pollIdx_ = 0;
    int startCalls_ = 0;
    bool stopCalled_ = false;
};
