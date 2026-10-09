#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "src/ui/desktop/connection/connection_ports.h"

namespace fastecu::ui
{

// Both fakes append to one log so a test can assert the order of effects
// across the launcher and the presentation.
class FakeIdentifyLauncher final : public IIdentifyLauncher
{
  public:
    explicit FakeIdentifyLauncher(std::vector<std::string>& log) : log_(log)
    {
    }

    void setCompletionHandler(CompletionHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void start(const diagnostics::SsmIdentifyRequest& request, IdentifyGeneration generation) override
    {
        log_.push_back("start " + std::to_string(generation));
        last_request = request;
        started_generations.push_back(generation);
    }

    void stopAndJoin() override
    {
        log_.push_back("stop_and_join");
    }

    // Test driver: deliver a completion the way the Qt launcher would.
    void complete(IdentifyGeneration generation, IdentifyOutcome outcome)
    {
        handler_(generation, std::move(outcome));
    }

    bool hasHandler() const
    {
        return static_cast<bool>(handler_);
    }

    diagnostics::SsmIdentifyRequest last_request;
    std::vector<IdentifyGeneration> started_generations;

  private:
    std::vector<std::string>& log_;
    CompletionHandler handler_;
};

class FakeConnectionPresentation final : public IConnectionPresentation
{
  public:
    explicit FakeConnectionPresentation(std::vector<std::string>& log) : log_(log)
    {
    }

    void setControlsLocked(bool locked) override
    {
        log_.push_back(std::string("controls_locked=") + (locked ? "1" : "0"));
    }

    void setPortSelectorEnabled(bool enabled) override
    {
        log_.push_back(std::string("port_selector=") + (enabled ? "1" : "0"));
    }

    void identified(const IdentifyOutcome& outcome) override
    {
        log_.push_back("identified " + outcome.ecu_id);
        if (on_identified)
        {
            on_identified(outcome);
        }
    }

    void identificationFailed(const IdentifyOutcome& outcome) override
    {
        log_.push_back("failed " + outcome.error_detail);
        if (on_failed)
        {
            on_failed(outcome);
        }
    }

    // Re-entry hooks: a test sets these to start or cancel a connection from
    // inside the callback, as capability parsing and disconnect do.
    std::function<void(const IdentifyOutcome&)> on_identified;
    std::function<void(const IdentifyOutcome&)> on_failed;

  private:
    std::vector<std::string>& log_;
};

} // namespace fastecu::ui
