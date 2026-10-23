#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// The Denso ISO-15765 bootloader dialect's executor: connect (OBK probe,
// identity queries, branch probe, then the in-car or bench arm and its kernel
// jump), then a read dump or a one-block erase, write and checksum close.
// Every way a family deviates is a value of its
// DensoIso15765BootloaderProfileOf<Plan>. Instantiated in the .cpp for the
// four bootloader-dialect plan types; each family's executor header names its
// instantiation.
template <typename Plan> class DensoIso15765BootloaderExecutor final : public ICanFlashExecutor
{
  public:
    Result<Iso15765Config> TransportSetup(const FlashPlan& plan) const override;

    Result<FlashExecutionResult> Execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
