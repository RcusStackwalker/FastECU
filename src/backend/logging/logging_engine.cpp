#include "src/backend/logging/logging_engine.h"

#include <utility>
#include "src/backend/logging/logging_worker.h"

LoggingEngine::LoggingEngine(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<QVector<LogSample>>();
    qRegisterMetaType<LoggingStatus>();
    qRegisterMetaType<SessionEndReason>();
}

LoggingEngine::~LoggingEngine()
{
    stop();
}

void LoggingEngine::registerProtocol(const QString& protocolId, LoggingProtocolFactory factory,
                                     int pollTimeoutMs, int carSilenceMissThreshold,
                                     int reconnectAttemptThreshold, int reconnectRetryPeriod)
{
    m_registrations.insert(protocolId, ProtocolRegistration{
                                           std::move(factory), pollTimeoutMs, carSilenceMissThreshold, reconnectAttemptThreshold, reconnectRetryPeriod});
}

bool LoggingEngine::isRunning() const
{
    return m_activeWorker != nullptr;
}

bool LoggingEngine::start(const LogSessionConfig& config)
{
    if (isRunning())
    {
        return false;
    }

    auto it = m_registrations.find(config.protocolId);
    if (it == m_registrations.end())
    {
        emit LOG_E("No logging protocol registered for '" + config.protocolId + "'", true, true);
        return false;
    }

    m_activeProtocol = it->factory(config);
    if (!m_activeProtocol)
    {
        emit LOG_E("Protocol factory for '" + config.protocolId + "' returned null", true, true);
        m_activeProtocol.reset();
        return false;
    }

    m_activeWorker = new LoggingWorker(m_activeProtocol.get(), it->pollTimeoutMs,
                                       it->carSilenceMissThreshold, it->reconnectAttemptThreshold,
                                       it->reconnectRetryPeriod, this);

    connect(m_activeWorker, &LoggingWorker::valuesUpdated, this, &LoggingEngine::valuesUpdated);
    connect(m_activeWorker, &LoggingWorker::statusChanged, this, &LoggingEngine::statusChanged);
    connect(m_activeWorker, &LoggingWorker::sessionEnded, this, &LoggingEngine::handleWorkerSessionEnded);
    connect(m_activeWorker, &LoggingWorker::LOG_E, this, &LoggingEngine::LOG_E);
    connect(m_activeWorker, &LoggingWorker::LOG_W, this, &LoggingEngine::LOG_W);
    connect(m_activeWorker, &LoggingWorker::LOG_I, this, &LoggingEngine::LOG_I);
    connect(m_activeWorker, &LoggingWorker::LOG_D, this, &LoggingEngine::LOG_D);

    m_activeWorker->start();
    return true;
}

void LoggingEngine::stop()
{
    if (!m_activeWorker)
    {
        return;
    }

    // Disconnect first: a user-requested stop should not also come back through
    // handleWorkerSessionEnded's sessionEnded(StoppedByUser, ...) relay -- the
    // caller already knows it asked for this.
    m_activeWorker->disconnect(this);
    m_activeWorker->requestStop();
    m_activeWorker->wait();
    delete m_activeWorker;
    m_activeWorker = nullptr;
    m_activeProtocol.reset();
}

void LoggingEngine::handleWorkerSessionEnded(SessionEndReason reason, QString message)
{
    emit sessionEnded(reason, std::move(message));
    if (m_activeWorker)
    {
        m_activeWorker->wait();
        m_activeWorker->deleteLater();
        m_activeWorker = nullptr;
        m_activeProtocol.reset();
    }
}
