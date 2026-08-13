#pragma once

#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

class SubaruDensoSh7055_02Executor final : public IFlashExecutor
{
  public:
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IFlashTransport& transport,
                                         IClock& clock, const ICancellationToken& cancellation,
                                         IEventSink& events) override;

  private:
    Status connect_bootloader(IKlineFlashTransport& transport, IClock& clock,
                              const ICancellationToken& cancellation, IEventSink& events,
                              const SubaruDensoSh7055_02Plan& family_plan, bool read_ecu_id,
                              bool& kernel_alive, std::optional<std::string>& ecu_id);
    Status upload_kernel(IKlineFlashTransport& transport, IClock& clock,
                         const ICancellationToken& cancellation, IEventSink& events,
                         const KernelImage& kernel);
    Result<bytes::Bytes> read_mem(IKlineFlashTransport& transport, IClock& clock,
                                  const ICancellationToken& cancellation, IEventSink& events,
                                  const MemoryRegion& region);
};

} // namespace fastecu::flash
