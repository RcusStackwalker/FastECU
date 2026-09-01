// Teardown, gate, and configuration-ordering coverage for
// ServiceFunctionWorker. Follows flash_worker_test.cpp: a FakeClock plus
// condition variables and thread joins, never QSignalSpy::wait(), so no
// assertion depends on wall-clock timing.
#include "src/platform/desktop/common/service_functions/service_function_worker.h"

#include <QCoreApplication>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
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

class GateObserver
{
  public:
    explicit GateObserver(ServiceFunctionWorker *worker)
    {
        QObject::connect(
            worker, &ServiceFunctionWorker::gateRequested, worker,
            [this](int)
            {
                const std::lock_guard lock(mutex_);
                ++count_;
                observed_.notify_all();
            },
            Qt::DirectConnection);
    }

    bool waitForCount(int expected)
    {
        std::unique_lock lock(mutex_);
        return observed_.wait_for(lock, std::chrono::seconds{5}, [this, expected] { return count_ >= expected; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable observed_;
    int count_ = 0;
};

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

struct BlockingLifetimeState
{
    QSemaphore entered;
    QSemaphore release;
    QSemaphore resume_exiting;
    std::atomic<bool> active{false};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> destroyed_while_active{false};
};

// Holds resume() beyond the worker's historical five-second destructor wait.
// The destructor's active-path release keeps the RED run deterministic and
// lets QThread exit cleanly after exposing premature owned-state destruction.
class BlockingLifetimeSession final : public ServiceFunctionSession
{
  public:
    explicit BlockingLifetimeSession(std::shared_ptr<BlockingLifetimeState> state) : state_(std::move(state))
    {
    }

    ~BlockingLifetimeSession() override
    {
        if (state_->active.load())
        {
            state_->destroyed_while_active.store(true);
            state_->release.release();
            state_->resume_exiting.tryAcquire(1, 1000);
            QThread::msleep(50); // let the worker finish before QThread teardown
        }
        state_->destroyed.store(true);
    }

    fastecu::Result<SsmTransportConfig> transport_setup() const override
    {
        return SsmTransportConfig{};
    }

    ServiceFunctionStep resume(ISsmTransport&, fastecu::IClock&, const fastecu::ICancellationToken&,
                               fastecu::IEventSink&) override
    {
        const std::shared_ptr<BlockingLifetimeState> state = state_;
        state->active.store(true);
        struct ActiveCall
        {
            std::shared_ptr<BlockingLifetimeState> state;
            ~ActiveCall()
            {
                state->active.store(false);
                state->resume_exiting.release();
            }
        } active_call{state};

        state->entered.release();
        state->release.acquire();
        return CompletedStep{SetParametersOutcome{}};
    }

    void submit(GateResponse) override
    {
    }

  private:
    std::shared_ptr<BlockingLifetimeState> state_;
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
        GateObserver gate_observer(harness.worker.get());
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QVERIFY(gate_observer.waitForCount(1));

        harness.worker->answerGate(static_cast<int>(OperatorGateId::RelearnEngineRunning), true);
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(gates.count(), 1);
        QCOMPARE(gates.at(0).at(0).toInt(), static_cast<int>(OperatorGateId::RelearnEngineRunning));
        QCOMPARE(harness.session->submitted, std::vector<GateResponse>{GateResponse::Accept});
        QCOMPARE(done.count(), 1);
    }

    void aDeclinedGateReachesTheSessionAsDecline()
    {
        Harness harness;
        harness.build({GateStep{OperatorGateId::RelearnStaticSetup},
                       FailedStep{fastecu::Error{ErrorKind::Cancelled, "operator declined a relearn gate"}}});
        GateObserver gate_observer(harness.worker.get());
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QVERIFY(gate_observer.waitForCount(1));
        harness.worker->answerGate(static_cast<int>(OperatorGateId::RelearnStaticSetup), false);
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(gates.count(), 1);
        QCOMPARE(gates.at(0).at(0).toInt(), static_cast<int>(OperatorGateId::RelearnStaticSetup));
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
        GateObserver gate_observer(harness.worker.get());
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QVERIFY(gate_observer.waitForCount(1));
        harness.worker->requestStop();
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(gates.count(), 1);
        QCOMPARE(gates.at(0).at(0).toInt(), static_cast<int>(OperatorGateId::RelearnEngineRunning));
        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::Cancelled);
    }

    void aStaleAnswerCannotSatisfyALaterGate()
    {
        Harness harness;
        harness.build({GateStep{OperatorGateId::RelearnStaticSetup}, GateStep{OperatorGateId::RelearnEngineRunning},
                       CompletedStep{SetParametersOutcome{}}});
        GateObserver gate_observer(harness.worker.get());
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->answerGate(static_cast<int>(OperatorGateId::RelearnStaticSetup), false);
        harness.worker->start();
        QVERIFY(gate_observer.waitForCount(1));

        harness.worker->answerGate(static_cast<int>(OperatorGateId::RelearnStaticSetup), true);
        QVERIFY(gate_observer.waitForCount(2));
        harness.worker->answerGate(static_cast<int>(OperatorGateId::RelearnStaticSetup), false);
        harness.worker->answerGate(static_cast<int>(OperatorGateId::RelearnEngineRunning), true);
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(gates.count(), 2);
        QCOMPARE(gates.at(0).at(0).toInt(), static_cast<int>(OperatorGateId::RelearnStaticSetup));
        QCOMPARE(gates.at(1).at(0).toInt(), static_cast<int>(OperatorGateId::RelearnEngineRunning));
        QCOMPARE(harness.session->submitted, std::vector<GateResponse>({GateResponse::Accept, GateResponse::Accept}));
        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QVERIFY(result.success);
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

    void destructorDoesNotDestroyOwnedStateWhileResumeIsActive()
    {
        auto state = std::make_shared<BlockingLifetimeState>();
        RecordingConfigurator configurator;
        auto worker = std::make_unique<ServiceFunctionWorker>(std::make_unique<BlockingLifetimeSession>(state),
                                                              std::make_unique<ScriptedSsmTransport>(),
                                                              std::make_unique<FakeClock>(), &configurator);
        worker->start();
        QVERIFY2(state->entered.tryAcquire(1, 1000), "blocking session did not enter resume()");

        std::atomic<bool> destructor_returned{false};
        std::thread destroyer(
            [owned = std::move(worker), &destructor_returned]() mutable
            {
                owned.reset();
                destructor_returned.store(true);
            });

        std::this_thread::sleep_for(std::chrono::milliseconds{5200});
        const bool destroyed_before_release = state->destroyed.load();
        const bool destructor_returned_before_release = destructor_returned.load();
        state->release.release();
        destroyer.join();

        QVERIFY(!destroyed_before_release);
        QVERIFY(!destructor_returned_before_release);
        QVERIFY(state->destroyed.load());
        QVERIFY(!state->destroyed_while_active.load());
    }
};

QTEST_MAIN(ServiceFunctionWorkerTest)
#include "service_function_worker_test.moc"
