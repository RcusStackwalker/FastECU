#pragma once

#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

class SubaruDensoSh7055_02Executor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;

  private:
    Status connect_bootloader(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const SubaruDensoSh7055_02Plan& family_plan, bool read_ecu_id,
                              bool& kernel_alive, std::optional<std::string>& ecu_id);
    Status upload_kernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                         IEventSink& events, const KernelImage& kernel);
    Result<bytes::Bytes> read_mem(IKlineFlashTransport& transport, IClock& clock,
                                  const ICancellationToken& cancellation, IEventSink& events,
                                  const MemoryRegion& region);
    Result<std::uint32_t> read_block_crc(IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, const MemoryRegion& block);
    Status flash_block(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                       IEventSink& events, bytes::ByteView image, const MemoryRegion& block, bool test_write);
    Status write_mem(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                     IEventSink& events, bytes::ByteView image, const std::string& mcu_name, bool test_write);
};

} // namespace fastecu::flash
