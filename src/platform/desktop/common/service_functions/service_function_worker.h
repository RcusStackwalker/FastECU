#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <memory>
#include <optional>

#include "src/backend/ports/clock.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/protocol/issm_transport.h"
#include "src/backend/service_functions/service_function_session.h"
#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"

namespace fastecu::service_functions
{

// Qt-friendly mirror of a terminal ServiceFunctionStep, for the same reason
// FlashWorkerResult exists: std::expected and std::variant are not Qt
// metatypes, and QSignalSpy / QueuedConnection copy through QVariant.
struct ServiceFunctionWorkerResult
{
    bool success = false;
    ErrorKind error_kind = ErrorKind::Internal;
    QString error_detail;
    std::optional<ServiceFunctionOutcome> outcome;
};

// Qt lifecycle adapter for a portable ServiceFunctionSession. Mirrors
// FlashWorker exactly except for the gate wait, which FlashWorker has no need
// for: flash collects every confirmation before execution, and relearn cannot.
class ServiceFunctionWorker final : public QThread
{
    Q_OBJECT

  public:
    ServiceFunctionWorker(std::unique_ptr<ServiceFunctionSession> session, std::unique_ptr<ISsmTransport> transport,
                          std::unique_ptr<IClock> clock, ISerialFacadeConfigurator *configurator,
                          QObject *parent = nullptr);
    ~ServiceFunctionWorker() override;

    ServiceFunctionWorker(const ServiceFunctionWorker&) = delete;
    ServiceFunctionWorker& operator=(const ServiceFunctionWorker&) = delete;

    // Cancels the token and wakes any outstanding gate wait. Safe from any
    // thread, any number of times, before or after start().
    void requestStop();

    // Answers the outstanding gate. Safe from any thread.
    void answerGate(bool accepted);

  signals:
    void logEvent(int level, QString message);
    void progressChanged(int done, int total);
    void gateRequested(int gateId);
    // Emitted exactly once per run(), always from this worker's own thread.
    void finished(fastecu::service_functions::ServiceFunctionWorkerResult result);

  protected:
    void run() override;

  private:
    // Blocks until answerGate() or requestStop(). Returns nullopt on stop.
    std::optional<GateResponse> waitForGate();

    std::unique_ptr<ServiceFunctionSession> session_;
    std::unique_ptr<ISsmTransport> transport_;
    std::unique_ptr<IClock> clock_;
    ISerialFacadeConfigurator *configurator_;
    ManualCancellationToken cancellation_;

    QMutex gate_mutex_;
    QWaitCondition gate_answered_;
    std::optional<GateResponse> gate_response_;
    bool gate_pending_ = false;
    bool stopping_ = false;
};

} // namespace fastecu::service_functions

Q_DECLARE_METATYPE(fastecu::service_functions::ServiceFunctionWorkerResult)
