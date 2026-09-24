#pragma once

#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Portable replacement for FlashEcuSubaruDensoSH705xKlineOperation (wave 6b-2).
class SubaruDensoSh705xKlineExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
    Status before_transport_configure(IKlineFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};

// upload_kernel(): append 00 00, pad to a multiple of four, drop the last two
// bytes, then append the big-endian 16-bit balance 0x5AA5 - (sum of the
// big-endian 32-bit words, mod 2^16). Legacy summed words with QByteArray::at()
// two bytes past the end, where the truncated padding still reads as zero;
// this treats those bytes as zero explicitly. Exposed for its golden test.
bytes::Bytes denso_sh705x_kline_balanced_kernel(bytes::ByteView kernel);

} // namespace fastecu::flash
