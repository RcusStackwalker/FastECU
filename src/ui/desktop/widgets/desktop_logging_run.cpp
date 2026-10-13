#include "src/ui/desktop/widgets/desktop_logging_run.h"

#include <utility>

namespace fastecu::desktop::logging
{
DesktopLoggingRun::DesktopLoggingRun(LoggingEngine& engine, LoggingCsvFile csvFile)
    : engine_(engine), csv_file_(std::move(csvFile))
{
    connect(&engine_, &LoggingEngine::valuesUpdated, this, &DesktopLoggingRun::handleSamples);
    connect(&engine_, &LoggingEngine::sessionEnded, this, &DesktopLoggingRun::handleCompletion);
}
DesktopLoggingRun::~DesktopLoggingRun()
{
    shutdown();
}
DesktopLoggingRunView DesktopLoggingRun::view() const
{
    return {active_snapshot_, values_, engine_.IsRunning(), csv_enabled_, csv_file_.IsOpen(), csv_path_};
}
std::optional<QString> DesktopLoggingRun::beginCsv(const QString& directory, const QString& nameStem)
{
    if (!csv_enabled_ || !active_snapshot_.has_value() || csv_failed_ || csv_file_.IsOpen())
    {
        return std::nullopt;
    }
    const auto opened = csv_file_.BeginRun(*active_snapshot_, directory, nameStem);
    if (!opened.has_value())
    {
        csv_failed_ = true;
        return QString::fromStdString(opened.error().detail);
    }
    csv_path_ = *opened;
    return std::nullopt;
}
Status DesktopLoggingRun::start(DesktopLoggingRunStart request)
{
    if (shutdown_ || active_snapshot_.has_value() || engine_.IsRunning())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    shutdown_ ? "logging run owner has shut down" : "a logging run is already active");
    }
    ++run_id_;
    active_snapshot_.emplace(std::move(request.snapshot));
    values_.BeginRun(*active_snapshot_);
    csv_failed_ = false;
    csv_path_.clear();
    elapsed_.start();
    const auto csvFailure = beginCsv(request.csv_directory, request.csv_name_stem);
    const auto started = engine_.Start(request.session, *active_snapshot_);
    if (!started.has_value())
    {
        const auto closeError = releaseRun();
        if (csvFailure.has_value())
        {
            reportCsvError(*csvFailure);
        }
        if (closeError.has_value())
        {
            reportCsvError(*closeError);
        }
        return started;
    }
    // Acquisition is established before observers can enter a modal CSV notice.
    if (csvFailure.has_value())
    {
        reportCsvError(*csvFailure);
    }
    if (active_snapshot_.has_value() && !shutdown_)
    {
        emit presentationChanged();
    }
    return {};
}
void DesktopLoggingRun::stop()
{
    // Engine completion is synchronous. Its subscriber may start a replacement;
    // never clean up or change flags after Stop returns.
    engine_.Stop();
}
void DesktopLoggingRun::setCsvEnabled(bool enabled, const QString& directory, const QString& nameStem)
{
    if (shutdown_ || enabled == csv_enabled_)
    {
        return;
    }
    csv_enabled_ = enabled;
    std::optional<QString> error;
    if (enabled)
    {
        csv_failed_ = false;
        error = beginCsv(directory, nameStem);
    }
    else
    {
        const auto closed = csv_file_.EndRun();
        if (!closed.has_value())
        {
            csv_failed_ = true;
            error = QString::fromStdString(closed.error().detail);
        }
    }
    if (error.has_value())
    {
        reportCsvError(*error);
    }
    if (!shutdown_)
    {
        emit presentationChanged();
    }
}
std::optional<QString> DesktopLoggingRun::releaseRun()
{
    ++run_id_;
    const auto closed = csv_file_.EndRun();
    active_snapshot_.reset();
    values_ = DesktopLoggerValues{};
    elapsed_.invalidate();
    if (!closed.has_value())
    {
        return QString::fromStdString(closed.error().detail);
    }
    return std::nullopt;
}
void DesktopLoggingRun::handleCompletion(SessionEndReason reason, const QString& message)
{
    if (!active_snapshot_.has_value())
    {
        return;
    }
    const auto closeError = releaseRun();
    if (shutdown_)
    {
        if (closeError.has_value())
        {
            emit shutdownError(*closeError);
        }
        return;
    }
    emit sessionEnded(reason, message);
    if (closeError.has_value())
    {
        reportCsvError(*closeError);
    }
}
void DesktopLoggingRun::reportCsvError(const QString& message)
{
    if (shutdown_)
    {
        emit shutdownError(message);
    }
    else
    {
        emit csvError(message);
    }
}
void DesktopLoggingRun::handleSamples(const QVector<fastecu::logging::LogSample>& samples)
{
    const auto runId = run_id_;
    const auto isCurrentRun = [this, runId] { return !shutdown_ && active_snapshot_.has_value() && run_id_ == runId; };
    if (!isCurrentRun())
    {
        return;
    }
    for (const auto& sample : samples)
    {
        if (!isCurrentRun())
        {
            return;
        }
        const auto applied = ApplyLogSample(*active_snapshot_, sample, values_);
        if (!applied.has_value())
        {
            emit sampleError(QString::fromStdString(applied.error().detail));
            if (!isCurrentRun())
            {
                return;
            }
        }
    }
    if (!isCurrentRun())
    {
        return;
    }
    // Rendering historically processes queued stop requests before CSV append.
    emit presentationChanged();
    if (!isCurrentRun() || !csv_enabled_ || csv_failed_ || !csv_file_.IsOpen())
    {
        return;
    }
    const auto written = csv_file_.AppendRow(values_, std::chrono::milliseconds{elapsed_.elapsed()});
    if (!written.has_value())
    {
        csv_failed_ = true;
        reportCsvError(QString::fromStdString(written.error().detail));
    }
}
void DesktopLoggingRun::shutdown()
{
    if (shutdown_)
    {
        return;
    }
    shutdown_ = true;
    disconnect(&engine_, nullptr, this, nullptr);
    engine_.Stop();
    const auto error = releaseRun();
    if (error.has_value())
    {
        emit shutdownError(*error);
    }
}
} // namespace fastecu::desktop::logging
