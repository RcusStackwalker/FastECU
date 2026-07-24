#include "src/platform/desktop/common/flash/flash_worker.h"

namespace fastecu::flash
{
namespace
{

class QtEventSinkForwarder final : public IEventSink
{
  public:
    explicit QtEventSinkForwarder(FlashWorker& worker) : worker_(worker)
    {
    }
    void log(LogLevel level, std::string_view message) override
    {
        emit worker_.logEvent(static_cast<int>(level),
                              QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
    }
    void progress(int done, int total) override
    {
        emit worker_.progressChanged(done, total);
    }
    void notice(std::string_view message) override
    {
        emit worker_.logEvent(static_cast<int>(LogLevel::Info),
                              QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
    }

  private:
    FlashWorker& worker_;
};

// Bounded, not absent: requestStop() (tripping cancellation_ and calling
// transport_->request_unblock()) is what actually makes run() return
// promptly, so this is a defensive backstop rather than the mechanism the
// teardown contract relies on. If it ever fires, that indicates a real
// executor/transport bug elsewhere (a blocking call that isn't honouring
// unblock/cancellation), not something to paper over with an unbounded
// wait() here.
constexpr unsigned long kTeardownWaitMs = 5000;

} // namespace

FlashWorker::FlashWorker(FlashPlan plan, std::unique_ptr<IFlashExecutor> executor,
                         std::unique_ptr<IFlashTransport> transport, std::unique_ptr<IClock> clock,
                         QObject *parent)
    : QThread(parent), plan_(std::move(plan)), executor_(std::move(executor)),
      transport_(std::move(transport)), clock_(std::move(clock))
{
    qRegisterMetaType<FlashWorkerResult>();
}

FlashWorker::~FlashWorker()
{
    requestStop();
    wait(kTeardownWaitMs);
}

void FlashWorker::requestStop()
{
    cancellation_.trip();
    transport_->request_unblock();
}

void FlashWorker::run()
{
    QtEventSinkForwarder events(*this);

    Result<FlashExecutionResult> result =
        executor_->execute(plan_, *transport_, *clock_, cancellation_.token(), events);

    FlashWorkerResult worker_result;
    if (result.has_value())
    {
        worker_result.success = true;
        worker_result.read_bytes = std::move(result->read_bytes);
    }
    else
    {
        worker_result.success = false;
        worker_result.error_kind = result.error().kind;
        worker_result.error_detail = QString::fromStdString(result.error().detail);
    }
    emit finished(worker_result);
}

} // namespace fastecu::flash
