#include "src/platform/desktop/common/flash/flash_worker.h"

#include "src/platform/desktop/common/ports/qt_event_sink.h"

namespace fastecu::flash
{
namespace
{

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
    // events is constructed here, so its thread affinity is this worker
    // thread. FlashWorker (this), however, is a QThread subclass -- and a
    // QThread object's own thread() is the thread that CONSTRUCTED it (the
    // caller of `new FlashWorker(...)`/start()), not the thread run()
    // executes on; QThread never moves itself onto the thread it manages.
    // So `this` and `events` are NOT on the same thread here, and plain
    // Qt::AutoConnection would resolve to a QueuedConnection (deferring the
    // re-emission of FlashWorker::logEvent/progressChanged onto whatever
    // thread constructed this FlashWorker, instead of firing synchronously
    // from run() the way the previous direct
    // `emit worker_.logEvent(...)` did). Forcing Qt::DirectConnection here
    // reproduces that prior synchronous-on-worker-thread behavior exactly,
    // so this is a pure wiring change with no timing/ordering difference --
    // not an accidental switch to queued delivery.
    QtEventSink events;
    connect(&events, &QtEventSink::logged, this, &FlashWorker::logEvent,
            Qt::DirectConnection);
    connect(&events, &QtEventSink::progressed, this, &FlashWorker::progressChanged,
            Qt::DirectConnection);
    connect(&events, &QtEventSink::phaseProgressed, this, &FlashWorker::phaseProgressChanged,
            Qt::DirectConnection);
    connect(&events, &QtEventSink::noticed, this, [this](QString message)
            { emit logEvent(static_cast<int>(LogLevel::Info), std::move(message)); }, Qt::DirectConnection);

    Result<FlashExecutionResult> result =
        executor_->execute(plan_, *transport_, *clock_, cancellation_.token(), events);

    FlashWorkerResult worker_result;
    if (result.has_value())
    {
        worker_result.success = true;
        worker_result.read_bytes = std::move(result->read_bytes);
        worker_result.rom_id = std::move(result->rom_id);
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
