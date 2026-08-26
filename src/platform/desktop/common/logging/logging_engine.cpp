#include "src/platform/desktop/common/logging/logging_engine.h"

#include <utility>

namespace fastecu::desktop::logging
{

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
    destroying_ = true;
    joinAndReleaseActiveRun();
}

bool LoggingEngine::isRunning() const
{
    return active_worker_ != nullptr;
}

fastecu::Status LoggingEngine::start(LoggingRun run)
{
    if (isRunning())
    {
        const fastecu::Error error{fastecu::ErrorKind::InvalidConfig, "a logging run is already active"};
        reportStartError(error);
        return std::unexpected(error);
    }

    if (!run.protocol)
    {
        const fastecu::Error error{fastecu::ErrorKind::InvalidConfig, "logging protocol is null"};
        reportStartError(error);
        return std::unexpected(error);
    }

    active_session_.emplace(std::move(run.session));
    active_protocol_ = std::move(run.protocol);

    last_status_.reset();
    worker_reached_running_ = false;
    explicit_stop_pending_ = false;
    completion_published_ = false;
    active_run_generation_ = ++next_run_generation_;
    active_worker_ = new LoggingWorker(*active_session_, active_protocol_.get(), clock_, diagnostics_, this);
    const std::uint64_t run_generation = active_run_generation_;
    connect(active_worker_, &LoggingWorker::samplesReady, this,
            [this, run_generation](QVector<fastecu::logging::LogSample> samples)
            {
                if (run_generation != active_run_generation_)
                {
                    return;
                }
                emit valuesUpdated(std::move(samples));
            });
    connect(active_worker_, &LoggingWorker::stateChanged, this,
            [this, run_generation](fastecu::logging::LoggingState state)
            {
                if (run_generation != active_run_generation_)
                {
                    return;
                }
                handleWorkerStateChanged(state);
            });
    connect(active_worker_, &LoggingWorker::sessionFinished, this,
            [this, run_generation](fastecu::Status result)
            {
                if (run_generation != active_run_generation_)
                {
                    return;
                }
                handleWorkerSessionFinished(std::move(result));
            });
    active_worker_->start();
    return {};
}

void LoggingEngine::stop()
{
    if (!active_worker_)
    {
        return;
    }

    explicit_stop_pending_ = true;
    finishActiveRun(SessionEndReason::StoppedByUser, {}, true);
    explicit_stop_pending_ = false;
}

void LoggingEngine::handleWorkerStateChanged(fastecu::logging::LoggingState state)
{
    const LoggingStatus status =
        state == fastecu::logging::LoggingState::Running ? LoggingStatus::Running : LoggingStatus::CarNotResponding;
    if (status == LoggingStatus::CarNotResponding)
    {
        emit LOG_W("Car not responding", true, true);
    }
    else if (last_status_ == LoggingStatus::CarNotResponding)
    {
        emit LOG_I("Car logging resumed", true, true);
    }
    last_status_ = status;
    if (state == fastecu::logging::LoggingState::Running)
    {
        worker_reached_running_ = true;
    }
    emit statusChanged(status);
}

void LoggingEngine::handleWorkerSessionFinished(fastecu::Status result)
{
    if (!active_worker_ || completion_published_ || destroying_)
    {
        return;
    }

    const bool reached_running = worker_reached_running_;
    SessionEndReason reason = SessionEndReason::RuntimeFailed;
    QString detail;

    if (result)
    {
        detail = "logging run ended without an error";
    }
    else
    {
        const fastecu::Error error = result.error();
        detail = QString::fromStdString(error.detail);
        if (error.kind == fastecu::ErrorKind::Cancelled)
        {
            reason = explicit_stop_pending_ ? SessionEndReason::StoppedByUser : SessionEndReason::RuntimeFailed;
        }
        else if (error.kind == fastecu::ErrorKind::Disconnected)
        {
            reason = SessionEndReason::AdapterDisconnected;
        }
        else
        {
            reason = reached_running ? SessionEndReason::RuntimeFailed : SessionEndReason::HandshakeFailed;
        }
    }

    finishActiveRun(reason, std::move(detail), true);
}

void LoggingEngine::finishActiveRun(SessionEndReason reason, QString detail, bool publish)
{
    joinAndReleaseActiveRun();
    if (publish && !destroying_)
    {
        publishCompletionOnce(reason, std::move(detail));
    }
}

void LoggingEngine::joinAndReleaseActiveRun()
{
    if (active_worker_)
    {
        active_worker_->disconnect(this);
        active_worker_->requestStop();
        active_worker_->wait();
        delete active_worker_;
        active_worker_ = nullptr;
    }
    active_run_generation_ = 0;
    active_protocol_.reset();
    active_session_.reset();
    last_status_.reset();
    worker_reached_running_ = false;
}

void LoggingEngine::publishCompletionOnce(SessionEndReason reason, QString detail)
{
    if (completion_published_ || destroying_)
    {
        return;
    }

    completion_published_ = true;
    switch (reason)
    {
    case SessionEndReason::StoppedByUser:
        break;
    case SessionEndReason::HandshakeFailed:
        emit LOG_E("Logging session failed to start: " + detail, true, true);
        break;
    case SessionEndReason::AdapterDisconnected:
        emit LOG_E("Adapter disconnected: " + detail, true, true);
        break;
    case SessionEndReason::RuntimeFailed:
        emit LOG_E("Logging session failed: " + detail, true, true);
        break;
    }
    emit sessionEnded(reason, std::move(detail));
}

void LoggingEngine::reportStartError(const fastecu::Error& error)
{
    emit LOG_E("Logging session failed to start: " + QString::fromStdString(error.detail), true, true);
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

} // namespace fastecu::desktop::logging
