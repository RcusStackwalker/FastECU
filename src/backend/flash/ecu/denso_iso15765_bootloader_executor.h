#pragma once

#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
#include "src/backend/flash/guarded_can_flash_transport.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"

namespace fastecu::flash
{

template <typename Plan> struct DensoIso15765CancellationPolicy
{
};

template <> struct DensoIso15765CancellationPolicy<SubaruDenso1n83m_1_5mCanPlan>
{
    using CancellationGuard = GuardedCanFlashTransport;
};

template <> struct DensoIso15765CancellationPolicy<SubaruDenso1n83m_4mCanPlan>
{
    using CancellationGuard = GuardedCanFlashTransport;
};

template <> struct DensoIso15765CancellationPolicy<SubaruDensoSh72531CanPlan>
{
    using CancellationGuard = GuardedCanFlashTransport;
};

template <> struct DensoIso15765CancellationPolicy<SubaruDensoSh72543CanDieselPlan>
{
    using CancellationGuard = GuardedCanFlashTransport;
};

// The Denso ISO-15765 bootloader dialect's executor: connect (OBK probe,
// identity queries, branch probe, then the in-car or bench arm and its kernel
// jump), then a read dump or a one-block erase, write and checksum close.
// Every way a family deviates is a value of its
// DensoIso15765BootloaderProfileOf<Plan>. Instantiated in the .cpp for the
// four bootloader-dialect plan types; each family's executor header names its
// instantiation.
template <typename Plan>
class DensoIso15765BootloaderExecutor final : public FamilyExecutor<Plan, ICanFlashExecutor>,
                                              public DensoIso15765CancellationPolicy<Plan>
{
  private:
    Result<Iso15765Config> Setup(const Plan& family) const override;

    Result<FlashExecutionResult> Run(const Plan& family, const FlashPlan& plan, ICanFlashTransport& transport,
                                     IClock& clock, const ICancellationToken& cancellation,
                                     IEventSink& events) override;
};

} // namespace fastecu::flash
