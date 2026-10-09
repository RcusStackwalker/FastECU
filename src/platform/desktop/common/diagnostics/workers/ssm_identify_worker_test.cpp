#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/diagnostics/workers/ssm_identify_worker.h"

#include <QCoreApplication>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <gtest/gtest.h>

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
        return fastecu::fail(ErrorKind::kCancelled);
    }

    std::atomic<bool> entered{false};
};
} // namespace

TEST(SsmIdentifyWorkerTest, stopsAtTheFirstSuccess)
{
    FakeDiagnosticLink link;
    link.queue_read(kShortEcuInit);
    auto clock = std::make_unique<FakeClock>();
    SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::kKlineSsm2, SsmTarget::kEcu}, link, std::move(clock));
    fastecu::testing::SignalRecorder done(&worker, &SsmIdentifyWorker::completed);
    worker.start();
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return done.count() != 0; }, std::chrono::milliseconds(5000)));
    worker.wait();
    ASSERT_EQ(done.count(), 1U);
    const auto result = std::get<0>(done.snapshot().at(0));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.ecu_id, QString("3152584006"));
    ASSERT_EQ(result.init_response.size(), qsizetype{14});
    ASSERT_EQ(opens(link), 1);
}

TEST(SsmIdentifyWorkerTest, retriesFiveTimesThenReportsTheLastError)
{
    FakeDiagnosticLink link;
    auto clock = std::make_unique<FakeClock>();
    FakeClock *clock_view = clock.get();
    SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::kKlineSsm2, SsmTarget::kEcu}, link, std::move(clock));
    fastecu::testing::SignalRecorder logs(&worker, &SsmIdentifyWorker::logEvent);
    fastecu::testing::SignalRecorder done(&worker, &SsmIdentifyWorker::completed);
    worker.start();
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return done.count() != 0; }, std::chrono::milliseconds(5000)));
    worker.wait();
    const auto result = std::get<0>(done.snapshot().at(0));
    ASSERT_TRUE(!result.success);
    ASSERT_EQ(result.error_kind, ErrorKind::kTimeout);
    ASSERT_EQ(opens(link), 5);
    ASSERT_EQ(logs.count(), 5U);
    // Five 200 ms settle sleeps inside the attempts and four 500 ms gaps
    // between them; no sleep after the last attempt.
    ASSERT_EQ(clock_view->elapsed().count(), 3000);
}

TEST(SsmIdentifyWorkerTest, stopBeforeStartCancelsAfterOneAttempt)
{
    FakeDiagnosticLink link;
    SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::kKlineSsm2, SsmTarget::kEcu}, link,
                             std::make_unique<FakeClock>());
    fastecu::testing::SignalRecorder done(&worker, &SsmIdentifyWorker::completed);
    worker.requestStop();
    worker.start();
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return done.count() != 0; }, std::chrono::milliseconds(5000)));
    worker.wait();
    ASSERT_EQ(std::get<0>(done.snapshot().at(0)).error_kind, ErrorKind::kCancelled);
    ASSERT_EQ(opens(link), 1);
}

TEST(SsmIdentifyWorkerTest, destroyingARunningWorkerJoinsIt)
{
    FakeDiagnosticLink link;
    BlockingClock *clock_view = nullptr;
    {
        auto clock = std::make_unique<BlockingClock>();
        clock_view = clock.get();
        SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::kKlineSsm2, SsmTarget::kEcu}, link, std::move(clock));
        worker.start();
        // Spin until the worker enters sleep(), proving run() is executing.
        ASSERT_TRUE(
            fastecu::testing::wait_until([&] { return clock_view->entered.load(); }, std::chrono::milliseconds(5000)));
    }
    // The destructor returned, so the thread has stopped touching the link.
    // The destructor called requestStop() which cancelled the sleep,
    // and called wait() which joined the thread.
    const auto calls_after = link.calls.size();
    fastecu::testing::process_events_for(std::chrono::milliseconds(50));
    ASSERT_EQ(link.calls.size(), calls_after);
    ASSERT_EQ(opens(link), 1); // Exactly one attempt started.
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
