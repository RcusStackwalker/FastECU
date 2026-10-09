#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/diagnostics/ssm_identify.h"

namespace fastecu::ui
{

// Identifies one start of an identification run. Every start and every cancel
// moves it forward, so a completion tagged with an older value is stale.
using IdentifyGeneration = std::uint64_t;

// Qt-free result of one identification run.
struct IdentifyOutcome
{
    bool success = false;
    std::string ecu_id;
    bytes::Bytes init_response;
    std::string error_detail;
};

// Runs identification off the UI thread. The Qt implementation wraps
// SsmIdentifyWorker; tests drive completions by hand.
class IIdentifyLauncher
{
  public:
    using CompletionHandler = std::function<void(IdentifyGeneration, IdentifyOutcome)>;

    virtual ~IIdentifyLauncher() = default;

    // Called once by the coordinator. A completion may be delivered after the
    // run it belongs to was stopped; it carries the generation it started with.
    virtual void setCompletionHandler(CompletionHandler handler) = 0;
    virtual void start(const diagnostics::SsmIdentifyRequest& request, IdentifyGeneration generation) = 0;
    // Stops and joins the current run, if any, and releases what it used.
    virtual void stopAndJoin() = 0;
};

// What a connection attempt shows and changes in the window. All calls are
// synchronous, on the UI thread.
class IConnectionPresentation
{
  public:
    virtual ~IConnectionPresentation() = default;

    // True while identification runs: the log-transport combo, the ECU/TCU
    // radio buttons and the Connect and Logging actions are disabled.
    virtual void setControlsLocked(bool locked) = 0;
    virtual void setPortSelectorEnabled(bool enabled) = 0;
    // Identification succeeded. May open a notice whose nested event loop
    // starts or cancels another identification.
    virtual void identified(const IdentifyOutcome& outcome) = 0;
    // Identification failed. The owner logs and disconnects.
    virtual void identificationFailed(const IdentifyOutcome& outcome) = 0;
};

} // namespace fastecu::ui
