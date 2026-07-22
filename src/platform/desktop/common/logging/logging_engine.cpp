#include "src/platform/desktop/common/logging/logging_engine.h"

#include <utility>

LoggingEngine::LoggingEngine(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<QVector<fastecu::logging::LogSample>>();
    qRegisterMetaType<fastecu::logging::LoggingState>();
    qRegisterMetaType<fastecu::Status>();
    qRegisterMetaType<LoggingStatus>();
    qRegisterMetaType<SessionEndReason>();
    connect(&diagnostics_, &QtEventSink::logged, this, &LoggingEngine::handleDiagnostic);
}

LoggingEngine::~LoggingEngine()
{
    stop();
}

void LoggingEngine::registerProtocol(const QString& protocol_id,
                                     LoggingProtocolFactory factory)
{
    registrations_.insert(protocol_id, std::move(factory));
}

bool LoggingEngine::isRunning() const
{
    return active_worker_ != nullptr;
}

bool LoggingEngine::start(
    LogSessionConfig config,
    fastecu::desktop::logging::DesktopLoggingSnapshot snapshot)
{
    if (isRunning())
    {
        return false;
    }

    const auto registration = registrations_.constFind(config.protocolId);
    if (registration == registrations_.constEnd())
    {
        emit LOG_E("No logging protocol registered for '" + config.protocolId + "'", true, true);
        return false;
    }

    active_snapshot_.emplace(std::move(snapshot));
    active_protocol_ = (*registration)(*active_snapshot_);
    if (!active_protocol_)
    {
        emit LOG_E("Protocol factory for '" + config.protocolId + "' returned null", true, true);
        active_snapshot_.reset();
        return false;
    }

    last_status_.reset();
    active_worker_ = new LoggingWorker(active_snapshot_->session, active_protocol_.get(),
                                       diagnostics_, this);
    connect(active_worker_, &LoggingWorker::samplesReady, this, &LoggingEngine::valuesUpdated);
    connect(active_worker_, &LoggingWorker::stateChanged, this,
            &LoggingEngine::handleWorkerStateChanged);
    connect(active_worker_, &LoggingWorker::sessionFinished, this,
            &LoggingEngine::handleWorkerSessionFinished);
    active_worker_->start();
    return true;
}

void LoggingEngine::stop()
{
    if (!active_worker_)
    {
        return;
    }

    active_worker_->disconnect(this);
    active_worker_->requestStop();
    active_worker_->wait();
    delete active_worker_;
    active_worker_ = nullptr;
    active_protocol_.reset();
    active_snapshot_.reset();
    last_status_.reset();
}

void LoggingEngine::handleWorkerStateChanged(fastecu::logging::LoggingState state)
{
    const LoggingStatus status = state == fastecu::logging::LoggingState::Running
                                     ? LoggingStatus::Running
                                     : LoggingStatus::CarNotResponding;
    if (status == LoggingStatus::CarNotResponding)
    {
        emit LOG_W("Car not responding", true, true);
    }
    else if (last_status_ == LoggingStatus::CarNotResponding)
    {
        emit LOG_I("Car logging resumed", true, true);
    }
    last_status_ = status;
    emit statusChanged(status);
}

void LoggingEngine::handleWorkerSessionFinished(fastecu::Status result)
{
    std::optional<fastecu::Error> error;
    if (!result)
    {
        error = result.error();
    }
    clearActiveSession();

    if (!error || error->kind == fastecu::ErrorKind::Cancelled)
    {
        return;
    }

    const QString message = QString::fromStdString(error->detail);
    if (error->kind == fastecu::ErrorKind::Disconnected)
    {
        emit LOG_E("Adapter disconnected: " + message, true, true);
        emit sessionEnded(SessionEndReason::AdapterDisconnected, message);
        return;
    }

    emit LOG_E("Logging session failed to start: " + message, true, true);
    emit sessionEnded(SessionEndReason::HandshakeFailed, message);
}

void LoggingEngine::handleDiagnostic(int level, QString message)
{
    switch (static_cast<fastecu::LogLevel>(level))
    {
    case fastecu::LogLevel::Error:
        emit LOG_E(std::move(message), true, true);
        break;
    case fastecu::LogLevel::Warning:
        emit LOG_W(std::move(message), true, true);
        break;
    case fastecu::LogLevel::Info:
        emit LOG_I(std::move(message), true, true);
        break;
    case fastecu::LogLevel::Debug:
        emit LOG_D(std::move(message), true, true);
        break;
    }
}

void LoggingEngine::clearActiveSession()
{
    if (active_worker_)
    {
        active_worker_->wait();
        active_worker_->deleteLater();
        active_worker_ = nullptr;
    }
    active_protocol_.reset();
    active_snapshot_.reset();
    last_status_.reset();
}
