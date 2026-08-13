#pragma once

#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

class SubaruDensoMc68hc16y5_02Executor final : public IFlashExecutor
{
  public:
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IFlashTransport& transport,
                                         IClock& clock, const ICancellationToken& cancellation,
                                         IEventSink& events) override;

  private:
    Status connect_bootloader(IKlineFlashTransport& transport, IClock& clock,
                              const ICancellationToken& cancellation, IEventSink& events,
                              const SubaruDensoMc68hc16y5_02Plan& family_plan, bool& kernel_alive);
    Status upload_kernel(IKlineFlashTransport& transport, IClock& clock,
                         const ICancellationToken& cancellation, IEventSink& events,
                         const SubaruDensoMc68hc16y5_02Plan& family_plan,
                         const KernelImage& kernel);
    Result<bytes::Bytes> read_mem(IKlineFlashTransport& transport, IClock& clock,
                                  const ICancellationToken& cancellation, IEventSink& events,
                                  const MemoryRegion& region);
    Result<std::uint32_t> read_block_crc(IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation,
                                         const MemoryRegion& block);
    Status flash_block(IKlineFlashTransport& transport, IClock& clock,
                       const ICancellationToken& cancellation, IEventSink& events,
                       bytes::ByteView image, const MemoryRegion& block, bool test_write);
    Status write_mem(IKlineFlashTransport& transport, IClock& clock,
                     const ICancellationToken& cancellation, IEventSink& events,
                     bytes::ByteView image, const std::string& mcu_name, bool test_write);
};

} // namespace fastecu::flash
