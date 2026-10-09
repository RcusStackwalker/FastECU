#include "src/ui/desktop/connection/connection_coordinator.h"

#include <utility>

namespace fastecu::ui
{

ConnectionCoordinator::ConnectionCoordinator(IIdentifyLauncher& launcher, IConnectionPresentation& presentation)
    : launcher_(launcher), presentation_(presentation)
{
    launcher_.setCompletionHandler([this](IdentifyGeneration generation, IdentifyOutcome outcome)
                                   { onCompleted(generation, std::move(outcome)); });
}

ConnectionCoordinator::~ConnectionCoordinator()
{
    launcher_.setCompletionHandler({});
}

void ConnectionCoordinator::begin(const diagnostics::SsmIdentifyRequest& request, std::function<void(bool)> onDone)
{
    pending_ = std::move(onDone);
    running_ = true;
    presentation_.setControlsLocked(true);
    launcher_.start(request, ++generation_);
}

void ConnectionCoordinator::cancel()
{
    ++generation_;
    if (!running_)
    {
        return;
    }
    launcher_.stopAndJoin();
    running_ = false;
    presentation_.setControlsLocked(false);
    // begin()'s caller locked the port selector once the port opened. A
    // cancelled identification leaves no ECU connected, so release it here,
    // whichever entry point cancelled.
    presentation_.setPortSelectorEnabled(true);
    if (auto done = std::exchange(pending_, {}); done)
    {
        done(false);
    }
}

void ConnectionCoordinator::shutdown()
{
    pending_ = nullptr;
    cancel();
}

void ConnectionCoordinator::onCompleted(IdentifyGeneration generation, IdentifyOutcome outcome)
{
    // Qt still delivers an event posted by a worker that was stopped, so a
    // completion tagged with an older generation is dropped. A completion for
    // the current generation with no run live is a duplicate.
    if (generation != generation_ || !running_)
    {
        return;
    }
    // Capability parsing in identified() can open a notice and re-enter the
    // connection flow. Take this attempt's continuation now so a nested
    // connection cannot clobber it.
    auto done = std::exchange(pending_, {});
    launcher_.stopAndJoin();
    running_ = false;
    if (outcome.success)
    {
        presentation_.identified(outcome);
    }
    else
    {
        presentation_.identificationFailed(outcome);
    }
    // A port that opened counts as connected even when identification failed.
    // A nested start or cancel during identified() moved the generation, which
    // cancels this attempt.
    if (done)
    {
        done(!outcome.success || generation == generation_);
    }
    // Keep the controls locked while a nested attempt is running; an older
    // completion must not unlock a newer attempt.
    if (!running_)
    {
        presentation_.setControlsLocked(false);
    }
}

} // namespace fastecu::ui
