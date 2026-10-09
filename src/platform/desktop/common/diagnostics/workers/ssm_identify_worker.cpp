#include "src/platform/desktop/common/diagnostics/workers/ssm_identify_worker.h"

#include <utility>

#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/backend/ports/event_sink.h"

namespace fastecu::diagnostics
{

SsmIdentifyWorker::SsmIdentifyWorker(SsmIdentifyRequest request, IDiagnosticLink& link, std::unique_ptr<IClock> clock,
                                     QObject *parent)
    : QThread(parent), request_(request), link_(link), clock_(std::move(clock))
{
    qRegisterMetaType<SsmIdentifyWorkerResult>();
}

SsmIdentifyWorker::~SsmIdentifyWorker()
{
    RequestStop();
    // run() uses owned members; join fully before they are destroyed.
    wait();
}

void SsmIdentifyWorker::RequestStop()
{
    cancellation_.Cancel();
}

void SsmIdentifyWorker::run()
{
    Result<SsmIdentity> outcome = Fail(ErrorKind::kInternal, "no identification attempt ran");
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt)
    {
        if (attempt > 1)
        {
            if (auto slept = clock_->Sleep(kRetryDelay, cancellation_); !slept.has_value())
            {
                outcome = std::unexpected(slept.error());
                break;
            }
        }
        outcome = IdentifySsmEcu(link_, *clock_, cancellation_, request_);
        if (outcome.has_value() || outcome.error().kind == ErrorKind::kCancelled)
        {
            break;
        }
        emit logEvent(static_cast<int>(LogLevel::kWarning), QString("ECU identification attempt %1 of %2 failed: %3")
                                                                .arg(attempt)
                                                                .arg(kMaxAttempts)
                                                                .arg(QString::fromStdString(outcome.error().detail)));
    }

    SsmIdentifyWorkerResult result;
    result.success = outcome.has_value();
    if (outcome.has_value())
    {
        result.ecu_id = QString::fromStdString(outcome->ecu_id);
        result.init_response = bytes::ToQByteArray(outcome->init_response);
    }
    else
    {
        result.error_kind = outcome.error().kind;
        result.error_detail = QString::fromStdString(outcome.error().detail);
    }
    emit completed(result);
}

} // namespace fastecu::diagnostics
