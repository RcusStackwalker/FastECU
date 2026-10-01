#pragma once
#include <QObject>
#include <QString>
#include <QThread>

#include <memory>
#include <optional>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/platform/desktop/common/flash/flash_workflow.h"

namespace fastecu::flash
{

// Qt-friendly mirror of Result<FlashExecutionResult>. A plain struct rather
// than Result<T> directly, since std::expected is not (and cannot cheaply be
// made) a Qt metatype -- QSignalSpy/QueuedConnection need to copy the
// argument through QVariant. error_kind/error_detail are meaningful only
// when !success; read_bytes is populated only on a successful Read.
struct FlashWorkerResult
{
    bool success = false;
    ErrorKind error_kind = ErrorKind::Internal;
    QString error_detail;
    std::optional<bytes::Bytes> read_bytes;
    std::optional<std::string> rom_id;
};

// Qt lifecycle adapter, not a base class for a portable executor. run()
// invokes exactly one synchronous executor call and exits; it never
// prompts, parses definitions, chooses EEPROM modes, mutates FileActions,
// classifies protocol replies, or implements retries -- all of that lives in
// the desktop dialog orchestration (Task 17) between FlashWorker runs.
//
// The clock is injected rather than constructed internally: this task does
// not own "what the real desktop clock is" (Task 17 wires a real one, e.g.
// QtClock from src/platform/desktop/common/ports), and tests must be able to
// substitute a fully deterministic fake (fastecu::FakeClock) so that the
// teardown contract this class exists for -- requestStop() unblocking a
// transport that is mid-read -- can be proven without any wall-clock
// dependency anywhere in the call chain.
class FlashWorker final : public QThread
{
    Q_OBJECT

  public:
    FlashWorker(FlashAttempt attempt, QObject *parent = nullptr);
    ~FlashWorker() override;

    FlashWorker(const FlashWorker&) = delete;
    FlashWorker& operator=(const FlashWorker&) = delete;

    // Cancels the cancellation token AND asks the transport to interrupt any
    // in-flight blocking call. Both halves are required: cancellation alone
    // only stops the executor at its next checkpoint (which may be behind a
    // still-blocked transport call), and request_unblock() alone doesn't
    // tell the executor the interruption means "stop", not "retry". Safe to
    // call from any thread, any number of times, before or after start().
    void requestStop();

  signals:
    void logEvent(int level, QString message);
    void progressChanged(int done, int total);
    void phaseProgressChanged(QString phaseName, int phaseIndex, int phaseCount, int done, int total);
    // Emitted exactly once per run(), always from this worker's own thread.
    void finished(fastecu::flash::FlashWorkerResult result);

  protected:
    void run() override;

  private:
    std::unique_ptr<BoundFlashAttempt> attempt_;
    std::unique_ptr<IClock> clock_;
    ManualCancellationToken cancellation_;
};

} // namespace fastecu::flash

Q_DECLARE_METATYPE(fastecu::flash::FlashWorkerResult)
