#include "src/platform/desktop/common/logging/runtime/logging_engine.h"

#include <exception>
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
    JoinAndReleaseActiveRun();
}

void LoggingEngine::RegisterProtocol(const QString& protocol_id, const LoggingProtocolFactory& factory)
{
    registrations_.insert(protocol_id, factory);
}

bool LoggingEngine::IsRunning() const
{
    return active_worker_ != nullptr;
}

fastecu::Status LoggingEngine::Start(const LogSessionConfig& config, DesktopLoggingSnapshot snapshot)
{
    if (IsRunning())
    {
        const fastecu::Error error{fastecu::ErrorKind::kInvalidConfig, "a logging run is already active"};
        ReportStartError(error);
        return std::unexpected(error);
    }

    const auto registration = registrations_.constFind(config.protocol_id);
    if (registration == registrations_.constEnd())
    {
        const fastecu::Error error{fastecu::ErrorKind::kInvalidConfig,
                                   "no logging protocol registered for '" + config.protocol_id.toStdString() + "'"};
        ReportStartError(error);
        return std::unexpected(error);
    }

    active_snapshot_.emplace(std::move(snapshot));
    fastecu::Result<std::unique_ptr<fastecu::logging::LoggingProtocol>> protocol_result;
    try
    {
        protocol_result = (*registration)(*active_snapshot_);
    }
    catch (const std::exception& error)
    {
        protocol_result = fastecu::Fail(fastecu::ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        protocol_result = fastecu::Fail(fastecu::ErrorKind::kInternal, "protocol factory threw an unknown exception");
    }
    if (!protocol_result)
    {
        const fastecu::Error error = protocol_result.error();
        active_snapshot_.reset();
        ReportStartError(error);
        return std::unexpected(error);
    }
    active_protocol_ = std::move(*protocol_result);
    if (!active_protocol_)
    {
        const fastecu::Error error{fastecu::ErrorKind::kInternal,
                                   "protocol factory for '" + config.protocol_id.toStdString() + "' returned null"};
        active_snapshot_.reset();
        ReportStartError(error);
        return std::unexpected(error);
    }

    last_status_.reset();
    worker_reached_running_ = false;
    explicit_stop_pending_ = false;
    completion_published_ = false;
    active_run_generation_ = ++next_run_generation_;
    active_worker_ = new LoggingWorker(active_snapshot_->session, active_protocol_.get(), diagnostics_, this);
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

void LoggingEngine::Stop()
{
    if (!active_worker_)
    {
        return;
    }

    explicit_stop_pending_ = true;
    FinishActiveRun(SessionEndReason::kStoppedByUser, {}, true);
    explicit_stop_pending_ = false;
}

void LoggingEngine::handleWorkerStateChanged(fastecu::logging::LoggingState state)
{
    const LoggingStatus status =
        state == fastecu::logging::LoggingState::kRunning ? LoggingStatus::kRunning : LoggingStatus::kCarNotResponding;
    if (status == LoggingStatus::kCarNotResponding)
    {
        emit logW("Car not responding", true, true);
    }
    else if (last_status_ == LoggingStatus::kCarNotResponding)
    {
        emit logI("Car logging resumed", true, true);
    }
    last_status_ = status;
    if (state == fastecu::logging::LoggingState::kRunning)
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
    SessionEndReason reason = SessionEndReason::kRuntimeFailed;
    QString detail;

    if (result)
    {
        detail = "logging run ended without an error";
    }
    else
    {
        const fastecu::Error error = result.error();
        detail = QString::fromStdString(error.detail);
        if (error.kind == fastecu::ErrorKind::kCancelled)
        {
            reason = explicit_stop_pending_ ? SessionEndReason::kStoppedByUser : SessionEndReason::kRuntimeFailed;
        }
        else if (error.kind == fastecu::ErrorKind::kDisconnected)
        {
            reason = SessionEndReason::kAdapterDisconnected;
        }
        else
        {
            reason = reached_running ? SessionEndReason::kRuntimeFailed : SessionEndReason::kHandshakeFailed;
        }
    }

    FinishActiveRun(reason, std::move(detail), true);
}

void LoggingEngine::FinishActiveRun(SessionEndReason reason, QString detail, bool publish)
{
    JoinAndReleaseActiveRun();
    if (publish && !destroying_)
    {
        PublishCompletionOnce(reason, std::move(detail));
    }
}

void LoggingEngine::JoinAndReleaseActiveRun()
{
    if (active_worker_)
    {
        active_worker_->disconnect(this);
        active_worker_->RequestStop();
        active_worker_->wait();
        delete active_worker_;
        active_worker_ = nullptr;
    }
    active_run_generation_ = 0;
    active_protocol_.reset();
    active_snapshot_.reset();
    last_status_.reset();
    worker_reached_running_ = false;
}

void LoggingEngine::PublishCompletionOnce(SessionEndReason reason, QString detail)
{
    if (completion_published_ || destroying_)
    {
        return;
    }

    completion_published_ = true;
    switch (reason)
    {
    case SessionEndReason::kStoppedByUser:
        break;
    case SessionEndReason::kHandshakeFailed:
        emit logE("Logging session failed to start: " + detail, true, true);
        break;
    case SessionEndReason::kAdapterDisconnected:
        emit logE("Adapter disconnected: " + detail, true, true);
        break;
    case SessionEndReason::kRuntimeFailed:
        emit logE("Logging session failed: " + detail, true, true);
        break;
    }
    emit sessionEnded(reason, std::move(detail));
}

void LoggingEngine::ReportStartError(const fastecu::Error& error)
{
    emit logE("Logging session failed to start: " + QString::fromStdString(error.detail), true, true);
}

void LoggingEngine::handleDiagnostic(int level, QString message)
{
    switch (static_cast<fastecu::LogLevel>(level))
    {
    case fastecu::LogLevel::kError:
        emit logE(std::move(message), true, true);
        break;
    case fastecu::LogLevel::kWarning:
        emit logW(std::move(message), true, true);
        break;
    case fastecu::LogLevel::kInfo:
        emit logI(std::move(message), true, true);
        break;
    case fastecu::LogLevel::kDebug:
        emit logD(std::move(message), true, true);
        break;
    }
}

} // namespace fastecu::desktop::logging
