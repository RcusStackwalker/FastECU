#include "src/platform/desktop/common/service_functions/service_function_worker.h"

#include <QMutexLocker>

#include <utility>

#include "src/platform/desktop/common/ports/qt_event_sink.h"

namespace fastecu::service_functions
{
namespace
{

constexpr unsigned long kTeardownWaitMs = 5000;

ServiceFunctionWorkerResult failureResult(const Error& error)
{
    return ServiceFunctionWorkerResult{
        .success = false,
        .error_kind = error.kind,
        .error_detail = QString::fromStdString(error.detail),
    };
}

} // namespace

ServiceFunctionWorker::ServiceFunctionWorker(std::unique_ptr<ServiceFunctionSession> session,
                                             std::unique_ptr<ISsmTransport> transport, std::unique_ptr<IClock> clock,
                                             ISerialFacadeConfigurator *configurator, QObject *parent)
    : QThread(parent), session_(std::move(session)), transport_(std::move(transport)), clock_(std::move(clock)),
      configurator_(configurator)
{
    qRegisterMetaType<ServiceFunctionWorkerResult>();
}

ServiceFunctionWorker::~ServiceFunctionWorker()
{
    requestStop();
    wait(kTeardownWaitMs);
}

void ServiceFunctionWorker::requestStop()
{
    cancellation_.cancel();
    const QMutexLocker lock(&gate_mutex_);
    stopping_ = true;
    gate_answered_.wakeAll();
}

void ServiceFunctionWorker::answerGate(bool accepted)
{
    const QMutexLocker lock(&gate_mutex_);
    gate_response_ = accepted ? GateResponse::Accept : GateResponse::Decline;
    gate_answered_.wakeAll();
}

std::optional<GateResponse> ServiceFunctionWorker::waitForGate()
{
    QMutexLocker lock(&gate_mutex_);
    while (!stopping_ && !gate_response_.has_value())
    {
        gate_answered_.wait(&gate_mutex_);
    }
    if (stopping_)
    {
        return std::nullopt;
    }
    return std::exchange(gate_response_, std::nullopt);
}

void ServiceFunctionWorker::run()
{
    const Result<SsmTransportConfig> setup = session_->transport_setup();
    if (!setup.has_value())
    {
        emit finished(failureResult(setup.error()));
        return;
    }

    if (const Status configured = configurator_->apply(*setup); !configured.has_value())
    {
        emit finished(failureResult(configured.error()));
        return;
    }

    QtEventSink events;
    connect(&events, &QtEventSink::logged, this, &ServiceFunctionWorker::logEvent, Qt::DirectConnection);
    connect(&events, &QtEventSink::progressed, this, &ServiceFunctionWorker::progressChanged, Qt::DirectConnection);
    connect(
        &events, &QtEventSink::noticed, this, [this](QString message)
        { emit logEvent(static_cast<int>(LogLevel::Info), std::move(message)); }, Qt::DirectConnection);

    while (true)
    {
        ServiceFunctionStep step = session_->resume(*transport_, *clock_, cancellation_, events);
        if (const auto *gate = std::get_if<GateStep>(&step); gate != nullptr)
        {
            emit gateRequested(static_cast<int>(gate->id));
            if (const std::optional<GateResponse> response = waitForGate(); response.has_value())
            {
                session_->submit(*response);
                continue;
            }
            emit finished(failureResult(Error{ErrorKind::Cancelled, "cancelled while waiting for operator gate"}));
            return;
        }
        if (auto *completed = std::get_if<CompletedStep>(&step); completed != nullptr)
        {
            ServiceFunctionWorkerResult result;
            result.success = true;
            result.outcome = std::move(completed->outcome);
            emit finished(result);
            return;
        }

        emit finished(failureResult(std::get<FailedStep>(step).error));
        return;
    }
}

} // namespace fastecu::service_functions
