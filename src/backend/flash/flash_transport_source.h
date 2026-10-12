#pragma once

#include <memory>

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// The connected adapter as the flash workflow sees it: it makes the K-Line,
// CAN or mixed-CAN flash transport an attempt is bound to, and reports whether
// the adapter supplies programming voltage itself. Each Make* returns a new,
// unopened transport; the bound attempt configures, opens and closes it
// (ADR 0015).
class IFlashTransportSource
{
  public:
    virtual ~IFlashTransportSource() = default;
    virtual std::unique_ptr<IKlineFlashTransport> MakeKline() = 0;
    virtual std::unique_ptr<ICanFlashTransport> MakeCan() = 0;
    virtual std::unique_ptr<IMixedCanFlashTransport> MakeMixedCan() = 0;
    // Whether the adapter drives programming voltage itself, so the operator
    // need not apply it.
    virtual bool SuppliesProgrammingVoltage() const = 0;
};

} // namespace fastecu::flash
