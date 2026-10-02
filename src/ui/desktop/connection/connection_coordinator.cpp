#include "src/ui/desktop/connection/connection_coordinator.h"

#include <utility>

namespace fastecu::ui
{

ConnectionCoordinator::ConnectionCoordinator(IIdentifyLauncher& launcher, IConnectionPresentation& presentation)
    : launcher_(launcher), presentation_(presentation)
{
    launcher_.set_completion_handler([this](IdentifyGeneration generation, IdentifyOutcome outcome)
                                     { on_completed(generation, std::move(outcome)); });
}

ConnectionCoordinator::~ConnectionCoordinator()
{
    launcher_.set_completion_handler({});
}

void ConnectionCoordinator::begin(const diagnostics::SsmIdentifyRequest& request, std::function<void(bool)> on_done)
{
    pending_ = std::move(on_done);
    running_ = true;
    presentation_.set_controls_locked(true);
    launcher_.start(request, ++generation_);
}

void ConnectionCoordinator::cancel()
{
    ++generation_;
    if (!running_)
    {
        return;
    }
    launcher_.stop_and_join();
    running_ = false;
    presentation_.set_controls_locked(false);
    // begin()'s caller locked the port selector once the port opened. A
    // cancelled identification leaves no ECU connected, so release it here,
    // whichever entry point cancelled.
    presentation_.set_port_selector_enabled(true);
    if (auto done = std::exchange(pending_, {}); done)
    {
        done(false);
    }
}

void ConnectionCoordinator::on_completed(IdentifyGeneration generation, IdentifyOutcome outcome)
{
    if (generation != generation_)
    {
        return;
    }
    auto done = std::exchange(pending_, {});
    launcher_.stop_and_join();
    running_ = false;
    if (outcome.success)
    {
        presentation_.identified(outcome);
    }
    else
    {
        presentation_.identification_failed(outcome);
    }
    if (done)
    {
        done(true);
    }
    presentation_.set_controls_locked(false);
}

} // namespace fastecu::ui
