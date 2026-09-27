#include "src/platform/desktop/common/diagnostics/ssm_identify_worker.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::IClock;
using fastecu::Status;
using fastecu::diagnostics::FakeDiagnosticLink;
using fastecu::diagnostics::SsmIdentifyRequest;
using fastecu::diagnostics::SsmIdentifyWorker;
using fastecu::diagnostics::SsmIdentifyWorkerResult;
using fastecu::diagnostics::SsmTarget;
using fastecu::diagnostics::SsmVariant;

namespace
{
const bytes::Bytes kShortEcuInit{0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x6C};

int opens(const FakeDiagnosticLink& link)
{
    return static_cast<int>(std::count_if(link.calls.begin(), link.calls.end(),
                                          [](const std::string& call) { return call.starts_with("open kline"); }));
}

// Clock that blocks in sleep() until cancellation is signalled, enabling
// deterministic testing of destructor joins.
class BlockingClock final : public IClock
{
  public:
    std::chrono::steady_clock::time_point now() const override
    {
        return std::chrono::steady_clock::now();
    }

    Status sleep(std::chrono::milliseconds /*duration*/, const fastecu::ICancellationToken& token) override
    {
        entered = true;
        while (!token.cancelled())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return fastecu::fail(ErrorKind::Cancelled);
    }

    std::atomic<bool> entered{false};
};
} // namespace

class SsmIdentifyWorkerTest : public QObject
{
    Q_OBJECT

  private slots:

    void stopsAtTheFirstSuccess()
    {
        FakeDiagnosticLink link;
        link.queue_read(kShortEcuInit);
        auto clock = std::make_unique<FakeClock>();
        SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::KlineSsm2, SsmTarget::Ecu}, link, std::move(clock));
        QSignalSpy done(&worker, &SsmIdentifyWorker::completed);
        worker.start();
        QVERIFY(done.wait(5000));
        worker.wait();
        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<SsmIdentifyWorkerResult>();
        QVERIFY(result.success);
        QCOMPARE(result.ecu_id, QString("3152584006"));
        QCOMPARE(result.init_response.size(), qsizetype{14});
        QCOMPARE(opens(link), 1);
    }

    void retriesFiveTimesThenReportsTheLastError()
    {
        FakeDiagnosticLink link;
        auto clock = std::make_unique<FakeClock>();
        FakeClock *clock_view = clock.get();
        SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::KlineSsm2, SsmTarget::Ecu}, link, std::move(clock));
        QSignalSpy logs(&worker, &SsmIdentifyWorker::logEvent);
        QSignalSpy done(&worker, &SsmIdentifyWorker::completed);
        worker.start();
        QVERIFY(done.wait(5000));
        worker.wait();
        const auto result = done.at(0).at(0).value<SsmIdentifyWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::Timeout);
        QCOMPARE(opens(link), 5);
        QCOMPARE(logs.count(), 5);
        // Five 200 ms settle sleeps inside the attempts and four 500 ms gaps
        // between them; no sleep after the last attempt.
        QCOMPARE(clock_view->elapsed().count(), 3000);
    }

    void stopBeforeStartCancelsAfterOneAttempt()
    {
        FakeDiagnosticLink link;
        SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::KlineSsm2, SsmTarget::Ecu}, link,
                                 std::make_unique<FakeClock>());
        QSignalSpy done(&worker, &SsmIdentifyWorker::completed);
        worker.requestStop();
        worker.start();
        QVERIFY(done.wait(5000));
        worker.wait();
        QCOMPARE(done.at(0).at(0).value<SsmIdentifyWorkerResult>().error_kind, ErrorKind::Cancelled);
        QCOMPARE(opens(link), 1);
    }

    void destroyingARunningWorkerJoinsIt()
    {
        FakeDiagnosticLink link;
        BlockingClock *clock_view = nullptr;
        {
            auto clock = std::make_unique<BlockingClock>();
            clock_view = clock.get();
            SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::KlineSsm2, SsmTarget::Ecu}, link, std::move(clock));
            worker.start();
            // Spin until the worker enters sleep(), proving run() is executing.
            QTRY_VERIFY(clock_view->entered);
        }
        // The destructor returned, so the thread has stopped touching the link.
        // The destructor called requestStop() which cancelled the sleep,
        // and called wait() which joined the thread.
        const auto calls_after = link.calls.size();
        QTest::qWait(50);
        QCOMPARE(link.calls.size(), calls_after);
        QCOMPARE(opens(link), 1); // Exactly one attempt started.
    }
};

QTEST_MAIN(SsmIdentifyWorkerTest)
#include "ssm_identify_worker_test.moc"
