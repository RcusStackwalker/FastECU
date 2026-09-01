// Teardown, gate, and configuration-ordering coverage for
// ServiceFunctionWorker. Follows flash_worker_test.cpp: a FakeClock plus
// condition variables and thread joins, never QSignalSpy::wait(), so no
// assertion depends on wall-clock timing.
#include "src/platform/desktop/common/service_functions/service_function_worker.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <memory>
#include <vector>

#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/scripted_ssm_transport.h"

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::service_functions::CompletedStep;
using fastecu::service_functions::FailedStep;
using fastecu::service_functions::GateResponse;
using fastecu::service_functions::GateStep;
using fastecu::service_functions::ISerialFacadeConfigurator;
using fastecu::service_functions::OperatorGateId;
using fastecu::service_functions::ServiceFunctionSession;
using fastecu::service_functions::ServiceFunctionStep;
using fastecu::service_functions::ServiceFunctionWorker;
using fastecu::service_functions::ServiceFunctionWorkerResult;
using fastecu::service_functions::SetParametersOutcome;
using fastecu::service_functions::SsmTransportConfig;

namespace
{

class RecordingConfigurator final : public ISerialFacadeConfigurator
{
  public:
    fastecu::Status apply(const SsmTransportConfig& config) override
    {
        applied.push_back(config);
        return {};
    }

    std::vector<SsmTransportConfig> applied;
};

// A session whose steps are supplied by the test, so worker behaviour is
// isolated from any real protocol.
class ScriptedSession final : public ServiceFunctionSession
{
  public:
    explicit ScriptedSession(std::vector<ServiceFunctionStep> steps, bool setup_fails = false)
        : steps_(std::move(steps)), setup_fails_(setup_fails)
    {
    }

    fastecu::Result<SsmTransportConfig> transport_setup() const override
    {
        if (setup_fails_)
        {
            return fastecu::fail(ErrorKind::Unsupported, "scripted setup failure");
        }
        return SsmTransportConfig{};
    }

    ServiceFunctionStep resume(ISsmTransport&, fastecu::IClock&, const fastecu::ICancellationToken& cancellation,
                               fastecu::IEventSink&) override
    {
        ++resume_calls;
        if (cancellation.cancelled())
        {
            return FailedStep{fastecu::Error{ErrorKind::Cancelled, "cancelled"}};
        }
        if (next_ >= steps_.size())
        {
            return FailedStep{fastecu::Error{ErrorKind::Internal, "script exhausted"}};
        }
        return steps_[next_++];
    }

    void submit(GateResponse response) override
    {
        submitted.push_back(response);
    }

    int resume_calls = 0;
    std::vector<GateResponse> submitted;

  private:
    std::vector<ServiceFunctionStep> steps_;
    bool setup_fails_;
    std::size_t next_ = 0;
};

struct Harness
{
    RecordingConfigurator configurator;
    ScriptedSession *session = nullptr;
    std::unique_ptr<ServiceFunctionWorker> worker;

    void build(std::vector<ServiceFunctionStep> steps, bool setup_fails = false)
    {
        auto owned = std::make_unique<ScriptedSession>(std::move(steps), setup_fails);
        session = owned.get();
        worker = std::make_unique<ServiceFunctionWorker>(std::move(owned), std::make_unique<ScriptedSsmTransport>(),
                                                         std::make_unique<FakeClock>(), &configurator);
    }
};

} // namespace

class ServiceFunctionWorkerTest : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        qRegisterMetaType<ServiceFunctionWorkerResult>("fastecu::service_functions::ServiceFunctionWorkerResult");
    }

    void completesWithoutEverRequestingAGate()
    {
        Harness harness;
        harness.build({CompletedStep{SetParametersOutcome{.frames_written = 12}}});
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(gates.count(), 0);
        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QVERIFY(result.success);
    }

    void appliesTheSessionsTransportConfigurationBeforeRunning()
    {
        Harness harness;
        harness.build({CompletedStep{SetParametersOutcome{}}});

        harness.worker->start();
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(harness.configurator.applied.size(), std::size_t{1});
        QCOMPARE(harness.configurator.applied.front(), SsmTransportConfig{});
    }

    void neverTouchesTheSerialFacadeWhenSetupFails()
    {
        Harness harness;
        harness.build({}, /*setup_fails=*/true);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QVERIFY(harness.worker->wait(5000));

        QVERIFY(harness.configurator.applied.empty());
        QCOMPARE(harness.session->resume_calls, 0);
        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::Unsupported);
    }

    void blocksOnAGateUntilItIsAnswered()
    {
        Harness harness;
        harness.build({GateStep{OperatorGateId::RelearnEngineRunning}, CompletedStep{SetParametersOutcome{}}});
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QTRY_COMPARE(gates.count(), 1);
        QCOMPARE(gates.at(0).at(0).toInt(), static_cast<int>(OperatorGateId::RelearnEngineRunning));
        QCOMPARE(done.count(), 0); // still parked on the gate
        QCOMPARE(harness.session->resume_calls, 1);

        harness.worker->answerGate(true);
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(harness.session->submitted, std::vector<GateResponse>{GateResponse::Accept});
        QCOMPARE(done.count(), 1);
    }

    void aDeclinedGateReachesTheSessionAsDecline()
    {
        Harness harness;
        harness.build({GateStep{OperatorGateId::RelearnStaticSetup},
                       FailedStep{fastecu::Error{ErrorKind::Cancelled, "operator declined a relearn gate"}}});
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QTRY_COMPARE(gates.count(), 1);
        harness.worker->answerGate(false);
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(harness.session->submitted, std::vector<GateResponse>{GateResponse::Decline});
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::Cancelled);
    }

    void requestStopUnblocksAnOutstandingGate()
    {
        // The teardown contract this class exists for: a worker parked on a
        // gate must not hold the thread open when the dialog closes.
        Harness harness;
        harness.build({GateStep{OperatorGateId::RelearnEngineRunning}, CompletedStep{SetParametersOutcome{}}});
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QTRY_COMPARE(gates.count(), 1);
        harness.worker->requestStop();
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::Cancelled);
    }

    void emitsFinishedExactlyOnceOnFailure()
    {
        Harness harness;
        harness.build({FailedStep{fastecu::Error{ErrorKind::BadResponse, "TCU said no"}}});
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QCOMPARE(result.error_kind, ErrorKind::BadResponse);
        QCOMPARE(result.error_detail, QString("TCU said no"));
    }
};

QTEST_MAIN(ServiceFunctionWorkerTest)
#include "service_function_worker_test.moc"
