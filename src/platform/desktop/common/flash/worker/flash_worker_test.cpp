#include "src/platform/desktop/common/testing/core_application_environment.h"
// Deterministic cancel/unblock/join teardown coverage for FlashWorker (step
// 5c, Task 11). Every plan built below uses build_denso_sh705x_eeprom_plan
// with a FakeClock injected into the worker -- both are real, already-tested
// portable components (Tasks 2/6), not stand-ins invented for this suite.
// The FakeClock is what makes the "blocked read" scenario deterministic: a
// real desktop clock's cancellable-but-still-real sleeps between
// connect_bootloader()'s protocol steps would race the test's own progress
// checks, defeating the point of proving the *transport* unblock (not a
// timing coincidence) is what makes teardown prompt. For the same reason the
// suite waits on condition variables and thread joins throughout, and never
// on signal recorder::wait() -- see the note in the first test.
#include "src/platform/desktop/common/flash/worker/flash_worker.h"
#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::flash::DensoSecurityVariant;
using fastecu::flash::DensoSh705xEepromInput;
using fastecu::flash::DensoSh705xEepromKlineExecutor;
using fastecu::flash::EepromReadMode;
using fastecu::flash::FlashAttempt;
using fastecu::flash::FlashFamily;
using fastecu::flash::FlashOperation;
using fastecu::flash::FlashWorker;
using fastecu::flash::FlashWorkerResult;
using fastecu::flash::KernelImage;
using fastecu::flash::MemoryRegion;
using fastecu::flash::ScriptedKlineFlashTransport;

namespace
{

class FakeBoundAttempt final : public fastecu::flash::BoundFlashAttempt
{
  public:
    explicit FakeBoundAttempt(fastecu::flash::FlashPlan plan) : plan_(std::move(plan))
    {
    }

    const fastecu::flash::FlashPlan& Plan() const noexcept override
    {
        return plan_;
    }

    fastecu::Result<fastecu::flash::FlashExecutionResult> Run(fastecu::IClock&, const fastecu::ICancellationToken&,
                                                              fastecu::IEventSink& events) override
    {
        ++run_calls;
        events.PhaseProgress(
            {.phase_name = "Connect to ECU", .phase_index = 1, .phase_count = 2, .done = 1, .total = 1});
        return fastecu::flash::FlashExecutionResult{};
    }

    void RequestUnblock() noexcept override
    {
        ++unblock_calls;
    }

    int run_calls = 0;
    int unblock_calls = 0;

  private:
    fastecu::flash::FlashPlan plan_;
};

// Every field here matches an SH7055 K-Line (or CAN, for the mismatch test)
// EEPROM read plan that build_denso_sh705x_eeprom_plan/validate_and_build
// accept outright -- see src/backend/flash/eeprom/denso_sh705x_eeprom_
// common.cpp's resolve_mcu_bounds("SH7055") for the eeprom/kernel-RAM bounds
// this must satisfy (eeprom = {0, 0x100}, kernel RAM = {0xFFFF6004, 0x6000}).
DensoSh705xEepromInput ValidInput(FlashFamily family)
{
    return DensoSh705xEepromInput{
        .operation = FlashOperation::kRead,
        .family = family,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_kline",
        .kernel = KernelImage{.id = "k", .load_address = 0xFFFF6004, .bytes = {0x01}},
        .mode = EepromReadMode::kMode2,
        .security = DensoSecurityVariant::kStock,
        .eeprom_region = MemoryRegion{.start = 0, .length = 0x100},
    };
}

// Byte-for-byte transcription of the anonymous-namespace
// request_kernel_id_frame() in denso_sh705x_eeprom_kline_executor.cpp: NOT
// ssm_protocol::addHeader-framed, unlike every other exchange in that file.
// connect_bootloader()'s very first action (after the bootloader-speed
// probe delay) is exactly this write, so ScriptedKlineFlashTransport must
// have it queued via expectWrite() before the ensuing read() can be reached
// at all -- otherwise the unscripted write itself fails with
// ErrorKind::kInternal before the blocking read is ever attempted, which
// would prove nothing about unblock/cancellation.
bytes::Bytes RequestKernelIdRequest()
{
    bytes::Bytes out{
        static_cast<bytes::Byte>((0xbeefU >> 8U) & 0xFFU), static_cast<bytes::Byte>(0xbeefU & 0xFFU), 0x00, 0x01, 0x01,
    };
    out.push_back(bytes::Sum8(out));
    return out;
}

} // namespace

TEST(TestFlashWorker, closingWhileReadIsBlocked_cancelsUnblocksAndJoinsWithoutWallClockSleep)
{
    auto plan = fastecu::flash::BuildDensoSh705xEepromPlan(ValidInput(FlashFamily::kDensoSh705xEepromKline));
    ASSERT_TRUE(plan.has_value());

    auto transport = std::make_unique<ScriptedKlineFlashTransport>();
    ScriptedKlineFlashTransport *raw_transport = transport.get();
    // connect_bootloader()'s initial kernel-alive probe: the write must
    // be scripted so it succeeds, so the ensuing read() is the one that
    // actually blocks.
    raw_transport->ExpectWrite(RequestKernelIdRequest());
    raw_transport->QueueBlockingRead();

    FlashWorker worker(FlashAttempt{fastecu::flash::BindFlashAttempt(std::move(*plan),
                                                                     std::make_unique<DensoSh705xEepromKlineExecutor>(),
                                                                     std::move(transport)),
                                    std::make_unique<FakeClock>()});
    fastecu::testing::SignalRecorder finished_spy(&worker, &FlashWorker::finished);

    worker.start();
    // Wait on the transport's own condition variable, not a fixed sleep:
    // requestStop() must land while read() is genuinely blocked for this
    // test to prove anything about unblocking.
    ASSERT_TRUE(raw_transport->WaitUntilBlockingReadEntered(std::chrono::milliseconds(2000)));
    worker.RequestStop();

    QElapsedTimer timer;
    timer.start();
    // wait() joins the worker thread, and finished is emitted as the last
    // act of run(), so the join is what makes the spy's contents final.
    // signal recorder::wait() must NOT be used here: the spy is connected with
    // Qt::DirectConnection and so records the emission on the worker
    // thread, which routinely wins the race to emit before wait() snapshots
    // its baseline count -- wait() is edge-triggered and reports only
    // emissions arriving strictly after that snapshot, so it would return
    // false after burning its full timeout.
    ASSERT_TRUE(worker.wait(2000));
    // The proof this test exists for: unblock is a condition-variable
    // wakeup inside the fake, not a wall-clock wait, so teardown
    // completes in well under the 2000ms test timeout budget.
    ASSERT_TRUE(timer.elapsed() < 500);

    ASSERT_EQ(finished_spy.Count(), 1U);
    auto result = std::get<0>(finished_spy.Snapshot().at(0));
    ASSERT_TRUE(!result.success);
    ASSERT_EQ(result.error_kind, ErrorKind::kCancelled);
    ASSERT_EQ(raw_transport->close_call_count, 1);
}

TEST(TestFlashWorker, oneAndOnlyOneTerminalResultIsEmitted)
{
    // A CAN-shaped plan handed to the K-Line executor: transport_setup()
    // rejects it before any I/O (zero writes/reads
    // scripted below, on purpose -- reaching the transport at all here
    // would itself be a bug).
    auto plan = fastecu::flash::BuildDensoSh705xEepromPlan(ValidInput(FlashFamily::kDensoSh705xEepromCan));
    ASSERT_TRUE(plan.has_value());

    auto transport = std::make_unique<ScriptedKlineFlashTransport>();
    FlashWorker worker(FlashAttempt{fastecu::flash::BindFlashAttempt(std::move(*plan),
                                                                     std::make_unique<DensoSh705xEepromKlineExecutor>(),
                                                                     std::move(transport)),
                                    std::make_unique<FakeClock>()});
    fastecu::testing::SignalRecorder finished_spy(&worker, &FlashWorker::finished);

    worker.start();
    // Joining is both necessary and sufficient: run() emits finished last,
    // so once the thread is joined the spy cannot gain further entries from
    // it. See the note in the test above for why signal recorder::wait() is the
    // wrong tool for "has this worker finished yet".
    ASSERT_TRUE(worker.wait(2000));
    // Give any (bug-induced) second emission from another path a chance to
    // arrive before asserting there is exactly one.
    fastecu::testing::ProcessEventsFor(std::chrono::milliseconds(50));

    ASSERT_EQ(finished_spy.Count(), 1U);
    auto result = std::get<0>(finished_spy.Snapshot().at(0));
    ASSERT_TRUE(!result.success);
    ASSERT_EQ(result.error_kind, ErrorKind::kInvalidConfig);
}

TEST(TestFlashWorker, phaseProgressIsForwardedAlongsideLegacyProgress)
{
    auto plan = fastecu::flash::BuildDensoSh705xEepromPlan(ValidInput(FlashFamily::kDensoSh705xEepromKline));
    ASSERT_TRUE(plan.has_value());

    auto attempt = std::make_unique<FakeBoundAttempt>(std::move(*plan));
    FlashWorker worker(FlashAttempt{std::move(attempt), std::make_unique<FakeClock>()});
    fastecu::testing::SignalRecorder legacy_spy(&worker, &FlashWorker::progressChanged);
    fastecu::testing::SignalRecorder phase_spy(&worker, &FlashWorker::phaseProgressChanged);

    worker.start();
    ASSERT_TRUE(worker.wait(2000));
    QCoreApplication::processEvents();

    ASSERT_EQ(legacy_spy.Count(), 1U);
    ASSERT_EQ(std::get<0>(legacy_spy.Snapshot().at(0)), 1);
    ASSERT_EQ(std::get<1>(legacy_spy.Snapshot().at(0)), 1);
    ASSERT_EQ(phase_spy.Count(), 1U);
    ASSERT_EQ(std::get<0>(phase_spy.Snapshot().at(0)), QString("Connect to ECU"));
    ASSERT_EQ(std::get<1>(phase_spy.Snapshot().at(0)), 1);
    ASSERT_EQ(std::get<2>(phase_spy.Snapshot().at(0)), 2);
    ASSERT_EQ(std::get<3>(phase_spy.Snapshot().at(0)), 1);
    ASSERT_EQ(std::get<4>(phase_spy.Snapshot().at(0)), 1);
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
