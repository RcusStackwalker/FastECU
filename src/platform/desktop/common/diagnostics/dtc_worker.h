#pragma once

#include <QString>
#include <QThread>

#include <memory>

#include "src/backend/diagnostics/dtc_session.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/protocol/idiagnostic_link.h"

namespace fastecu::diagnostics
{

// Qt-friendly outcome: std::expected is not a Qt metatype.
struct DtcWorkerResult
{
    bool success = false;
    ErrorKind error_kind = ErrorKind::Internal;
    QString error_detail;
};

// Runs one run_dtc_session on its own thread. Mirrors ServiceFunctionWorker
// without operator gates. The link is not owned and must outlive the worker.
class DtcWorker final : public QThread
{
    Q_OBJECT

  public:
    DtcWorker(DtcRequest request, IDiagnosticLink& link, std::unique_ptr<IClock> clock, QObject *parent = nullptr);
    ~DtcWorker() override;

    DtcWorker(const DtcWorker&) = delete;
    DtcWorker& operator=(const DtcWorker&) = delete;

    // Safe from any thread, any number of times, before or after start().
    void requestStop();

  signals:
    void logEvent(int level, QString message);
    // Emitted exactly once per run(), from the worker thread. Named
    // `completed` so it does not overload QThread::finished().
    void completed(fastecu::diagnostics::DtcWorkerResult result);

  protected:
    void run() override;

  private:
    DtcRequest request_;
    IDiagnosticLink& link_;
    std::unique_ptr<IClock> clock_;
    ManualCancellationToken cancellation_;
};

} // namespace fastecu::diagnostics

Q_DECLARE_METATYPE(fastecu::diagnostics::DtcWorkerResult)
