#include "src/ui/desktop/widgets/qt_identify_launcher.h"

#include <utility>

#include "src/platform/desktop/common/bytes/qt_bytes.h"

namespace fastecu::ui
{

namespace
{

IdentifyOutcome to_outcome(const diagnostics::SsmIdentifyWorkerResult& result)
{
    IdentifyOutcome outcome;
    outcome.success = result.success;
    outcome.ecu_id = result.ecu_id.toStdString();
    outcome.init_response = bytes::FromQByteArray(result.init_response);
    outcome.error_detail = result.error_detail.toStdString();
    return outcome;
}

} // namespace

QtIdentifyLauncher::QtIdentifyLauncher(LinkFactory makeLink, ClockFactory makeClock, LogHandler log, QObject *parent)
    : QObject(parent), make_link_(std::move(makeLink)), make_clock_(std::move(makeClock)), log_(std::move(log))
{
}

QtIdentifyLauncher::~QtIdentifyLauncher() = default;

void QtIdentifyLauncher::set_completion_handler(CompletionHandler handler)
{
    handler_ = std::move(handler);
}

void QtIdentifyLauncher::start(const diagnostics::SsmIdentifyRequest& request, IdentifyGeneration generation)
{
    link_ = make_link_();
    worker_ = std::make_unique<diagnostics::SsmIdentifyWorker>(request, *link_, make_clock_());
    connect(
        worker_.get(), &diagnostics::SsmIdentifyWorker::logEvent, this,
        [this](int level, const QString& message)
        {
            if (log_)
            {
                log_(static_cast<LogLevel>(level), message);
            }
        },
        Qt::QueuedConnection);
    connect(
        worker_.get(), &diagnostics::SsmIdentifyWorker::completed, this,
        [this, generation](const diagnostics::SsmIdentifyWorkerResult& result)
        {
            if (handler_)
            {
                handler_(generation, to_outcome(result));
            }
        },
        Qt::QueuedConnection);
    worker_->start();
}

void QtIdentifyLauncher::stop_and_join()
{
    if (!worker_)
    {
        return;
    }
    worker_->RequestStop();
    worker_->wait();
    worker_.reset();
    link_.reset();
}

bool QtIdentifyLauncher::wait_for_worker(std::chrono::milliseconds timeout)
{
    return !worker_ || worker_->wait(static_cast<unsigned long>(timeout.count()));
}

} // namespace fastecu::ui
