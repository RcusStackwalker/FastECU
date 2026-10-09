#pragma once

#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

class SubaruDensoSh7055_02Executor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> TransportSetup(const FlashPlan& plan) const override;
    Status BeforeTransportOpen(const ICancellationToken& cancellation) const override;
    Result<FlashExecutionResult> Execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;

  private:
    Status ConnectBootloader(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                             IEventSink& events, const SubaruDensoSh7055_02Plan& family_plan, bool read_ecu_id,
                             bool& kernel_alive, std::optional<std::string>& ecu_id) const;
    Status UploadKernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                        IEventSink& events, const KernelImage& kernel) const;
    Result<bytes::Bytes> ReadMem(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                                 IEventSink& events, const MemoryRegion& region) const;
    Result<std::uint32_t> ReadBlockCrc(IKlineFlashTransport& transport, IClock& clock,
                                       const ICancellationToken& cancellation, const MemoryRegion& block) const;
    Status FlashBlock(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                      IEventSink& events, bytes::ByteView image, const MemoryRegion& block, bool test_write) const;
    Status WriteMem(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                    IEventSink& events, bytes::ByteView image, const std::string& mcu_name, bool test_write) const;
};

} // namespace fastecu::flash
