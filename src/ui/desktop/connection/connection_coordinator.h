#pragma once

#include <functional>

#include "src/ui/desktop/connection/connection_ports.h"

namespace fastecu::ui
{

// Sequences ECU identification for one window: starting a run, cancelling it
// from any entry point, and delivering the result. Owns the generation fence
// that drops completions queued by a run that was stopped, the continuation of
// the attempt in flight, and whether a run is live. Borrows the launcher and
// the presentation; both must outlive it and must not be moved.
class ConnectionCoordinator
{
  public:
    ConnectionCoordinator(IIdentifyLauncher& launcher, IConnectionPresentation& presentation);
    ~ConnectionCoordinator();

    ConnectionCoordinator(const ConnectionCoordinator&) = delete;
    ConnectionCoordinator& operator=(const ConnectionCoordinator&) = delete;

    // The port is open and the SSM variant is resolved. Locks the controls and
    // starts a run. Callers cancel() first. on_done(false) means the attempt
    // was cancelled; on_done(true) means the port opened, whether or not the
    // ECU answered.
    //
    // on_done(false) can run synchronously inside cancel(), before the caller
    // of cancel() goes on to use the serial facade. on_done must therefore not
    // start a connection or otherwise touch the facade synchronously; defer any
    // such work to the event loop.
    void begin(const diagnostics::SsmIdentifyRequest& request, std::function<void(bool)> on_done);

    // Stops and joins a running identification, unlocks the controls and the
    // port selector, and tells a waiting caller the connect did not complete.
    // Always advances the generation, even when idle, so a completion queued
    // before a disconnect is dropped.
    void cancel();

    bool identifying() const
    {
        return running_;
    }

  private:
    void on_completed(IdentifyGeneration generation, IdentifyOutcome outcome);

    IIdentifyLauncher& launcher_;
    IConnectionPresentation& presentation_;
    IdentifyGeneration generation_ = 0;
    bool running_ = false;
    std::function<void(bool)> pending_;
};

} // namespace fastecu::ui
