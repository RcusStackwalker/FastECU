#pragma once

#include <QByteArray>
#include <QString>
#include <QThread>

#include <chrono>
#include <memory>

#include "src/backend/diagnostics/ssm_identify.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/protocol/idiagnostic_link.h"

namespace fastecu::diagnostics
{

// Qt-friendly outcome: std::expected is not a Qt metatype.
struct SsmIdentifyWorkerResult
{
    bool success = false;
    ErrorKind error_kind = ErrorKind::kInternal;
    QString error_detail;
    QString ecu_id;
    QByteArray init_response;
};

// Runs connect_to_ecu's identification retry loop on its own thread. Shaped
// like DtcWorker. The link is not owned and must outlive the worker.
class SsmIdentifyWorker final : public QThread
{
    Q_OBJECT

  public:
    static constexpr int kMaxAttempts = 5;
    static constexpr std::chrono::milliseconds kRetryDelay{500};

    SsmIdentifyWorker(SsmIdentifyRequest request, IDiagnosticLink& link, std::unique_ptr<IClock> clock,
                      QObject *parent = nullptr);
    ~SsmIdentifyWorker() override;

    SsmIdentifyWorker(const SsmIdentifyWorker&) = delete;
    SsmIdentifyWorker& operator=(const SsmIdentifyWorker&) = delete;

    // Safe from any thread, any number of times, before or after start().
    void RequestStop();

  signals:
    void logEvent(int level, QString message);
    // Emitted exactly once per run(), from the worker thread.
    void completed(fastecu::diagnostics::SsmIdentifyWorkerResult result);

  protected:
    void run() override;

  private:
    SsmIdentifyRequest request_;
    IDiagnosticLink& link_;
    std::unique_ptr<IClock> clock_;
    ManualCancellationToken cancellation_;
};

} // namespace fastecu::diagnostics

Q_DECLARE_METATYPE(fastecu::diagnostics::SsmIdentifyWorkerResult)
