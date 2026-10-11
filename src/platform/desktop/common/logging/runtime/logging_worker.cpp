#include "src/platform/desktop/common/logging/runtime/logging_worker.h"

#include <span>
#include <utility>

namespace fastecu::desktop::logging
{

namespace
{

class QtLoggingEventSink final : public fastecu::logging::ILoggingEventSink
{
  public:
    explicit QtLoggingEventSink(LoggingWorker *worker) : worker_(worker)
    {
    }

    void StateChanged(fastecu::logging::LoggingState state) override
    {
        emit worker_->stateChanged(state);
    }

    void Samples(std::span<const fastecu::logging::LogSample> samples) override
    {
        QVector<fastecu::logging::LogSample> values;
        values.reserve(static_cast<qsizetype>(samples.size()));
        for (const auto& sample : samples)
        {
            values.push_back(sample);
        }
        emit worker_->samplesReady(std::move(values));
    }

  private:
    LoggingWorker *worker_;
};

} // namespace

LoggingWorker::LoggingWorker(fastecu::logging::LoggingSession session, fastecu::logging::LoggingProtocol *protocol,
                             fastecu::IEventSink& diagnostics, QObject *parent)
    : QThread(parent), session_(std::move(session)), protocol_(protocol), diagnostics_(diagnostics)
{
}

LoggingWorker::~LoggingWorker()
{
    RequestStop();
    wait();
}

void LoggingWorker::RequestStop()
{
    cancellation_.Cancel();
}

void LoggingWorker::run()
{
    QtLoggingEventSink events{this};
    result_ = use_case_.Run(session_, *protocol_, cancellation_, events, diagnostics_);
    emit sessionFinished(result_);
}

} // namespace fastecu::desktop::logging
