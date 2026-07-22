#include "src/backend/logging/logging_worker.h"

LoggingWorker::LoggingWorker(LoggingProtocol *protocol, int pollTimeoutMs,
                             int carSilenceMissThreshold, int reconnectAttemptThreshold,
                             int reconnectRetryPeriod, QObject *parent)
    : QThread(parent), m_protocol(protocol), m_pollTimeoutMs(pollTimeoutMs), m_carSilenceMissThreshold(carSilenceMissThreshold), m_reconnectAttemptThreshold(reconnectAttemptThreshold), m_reconnectRetryPeriod(reconnectRetryPeriod)
{
}

LoggingWorker::~LoggingWorker()
{
    requestStop();
    wait();
}

void LoggingWorker::requestStop()
{
    m_stopRequested.storeRelaxed(true);
}

void LoggingWorker::run()
{
    if (auto s = m_protocol->start(); !s)
    {
        QString err = QString::fromStdString(s.error().detail);
        emit LOG_E("Logging session failed to start: " + err, true, true);
        emit sessionEnded(SessionEndReason::HandshakeFailed, err);
        return;
    }

    emit statusChanged(LoggingStatus::Running);
    LoggingStatus lastStatus = LoggingStatus::Running;
    int consecutiveMisses = 0;

    while (!m_stopRequested.loadRelaxed())
    {
        fastecu::Result<PollData> r = m_protocol->poll(m_pollTimeoutMs);
        if (!r)
        {
            m_protocol->stop();
            QString msg = QString::fromStdString(r.error().detail);
            emit LOG_E("Adapter disconnected: " + msg, true, true);
            emit sessionEnded(SessionEndReason::AdapterDisconnected, msg);
            return;
        }
        if (r->responded)
        {
            consecutiveMisses = 0;
            if (lastStatus != LoggingStatus::Running)
            {
                lastStatus = LoggingStatus::Running;
                emit statusChanged(LoggingStatus::Running);
            }
            emit valuesUpdated(r->samples);
        }
        else
        {
            ++consecutiveMisses;
            if (consecutiveMisses == m_carSilenceMissThreshold)
            {
                lastStatus = LoggingStatus::CarNotResponding;
                emit LOG_W("Car not responding", true, true);
                emit statusChanged(LoggingStatus::CarNotResponding);
            }
            if (m_reconnectRetryPeriod > 0 && consecutiveMisses >= m_reconnectAttemptThreshold && (consecutiveMisses - m_reconnectAttemptThreshold) % m_reconnectRetryPeriod == 0)
            {
                if (m_protocol->start())
                {
                    consecutiveMisses = 0;
                    lastStatus = LoggingStatus::Running;
                    emit LOG_I("Car logging resumed", true, true);
                    emit statusChanged(LoggingStatus::Running);
                }
            }
        }
    }

    m_protocol->stop();
    emit sessionEnded(SessionEndReason::StoppedByUser, QString());
}
