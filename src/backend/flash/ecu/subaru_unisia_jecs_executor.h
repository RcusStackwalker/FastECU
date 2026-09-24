#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruUnisiaJecsExecutorTestPeer;

class SubaruUnisiaJecsExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;

  private:
    friend class SubaruUnisiaJecsExecutorTestPeer;
    static Result<bytes::Bytes> read_range(std::uint32_t begin, std::uint32_t end, IKlineFlashTransport& transport,
                                           IClock& clock, const ICancellationToken& cancellation, IEventSink& events);
};
} // namespace fastecu::flash
