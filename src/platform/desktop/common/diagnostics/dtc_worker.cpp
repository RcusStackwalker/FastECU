#include "src/platform/desktop/common/diagnostics/dtc_worker.h"

#include <utility>

#include "src/platform/desktop/common/ports/qt_event_sink.h"

namespace fastecu::diagnostics
{

DtcWorker::DtcWorker(DtcRequest request, IDiagnosticLink& link, std::unique_ptr<IClock> clock, QObject *parent)
    : QThread(parent), request_(request), link_(link), clock_(std::move(clock))
{
    qRegisterMetaType<DtcWorkerResult>();
}

DtcWorker::~DtcWorker()
{
    requestStop();
    // run() uses owned members; join fully before they are destroyed.
    wait();
}

void DtcWorker::requestStop()
{
    cancellation_.cancel();
}

void DtcWorker::run()
{
    QtEventSink events;
    connect(&events, &QtEventSink::logged, this, &DtcWorker::logEvent, Qt::DirectConnection);
    connect(
        &events, &QtEventSink::noticed, this, [this](QString message)
        { emit logEvent(static_cast<int>(LogLevel::Info), std::move(message)); }, Qt::DirectConnection);

    const Result<DtcReport> report = run_dtc_session(request_, link_, *clock_, cancellation_, events);
    DtcWorkerResult result;
    result.success = report.has_value();
    if (!report.has_value())
    {
        result.error_kind = report.error().kind;
        result.error_detail = QString::fromStdString(report.error().detail);
    }
    emit completed(result);
}

} // namespace fastecu::diagnostics
