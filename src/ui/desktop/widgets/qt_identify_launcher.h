#pragma once

#include <QObject>
#include <QString>

#include <chrono>
#include <functional>
#include <memory>

#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/protocol/idiagnostic_link.h"
#include "src/platform/desktop/common/diagnostics/workers/ssm_identify_worker.h"
#include "src/ui/desktop/connection/connection_ports.h"

namespace fastecu::ui
{

// SsmIdentifyWorker behind IIdentifyLauncher. The worker's completion is a
// queued signal, so it is delivered on the UI thread after the run returned; it
// can also arrive after stop_and_join() joined the run, and carries the
// generation its run started with so the coordinator can drop it.
class QtIdentifyLauncher final : public QObject, public IIdentifyLauncher
{
    Q_OBJECT

  public:
    using LinkFactory = std::function<std::unique_ptr<diagnostics::IDiagnosticLink>()>;
    using ClockFactory = std::function<std::unique_ptr<IClock>()>;
    using LogHandler = std::function<void(LogLevel, const QString&)>;

    QtIdentifyLauncher(LinkFactory makeLink, ClockFactory makeClock, LogHandler log, QObject *parent = nullptr);
    ~QtIdentifyLauncher() override;

    void setCompletionHandler(CompletionHandler handler) override;
    void start(const diagnostics::SsmIdentifyRequest& request, IdentifyGeneration generation) override;
    void stopAndJoin() override;

    // Test hook: waits for the worker thread to finish without consuming the
    // completion it queued. True when no worker is live or it finished in time.
    bool waitForWorker(std::chrono::milliseconds timeout);

  private:
    LinkFactory make_link_;
    ClockFactory make_clock_;
    LogHandler log_;
    CompletionHandler handler_;
    // Declared link first, so the worker (which uses it) is destroyed first.
    std::unique_ptr<diagnostics::IDiagnosticLink> link_;
    std::unique_ptr<diagnostics::SsmIdentifyWorker> worker_;
};

} // namespace fastecu::ui
