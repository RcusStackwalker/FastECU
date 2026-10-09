#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruUnisiaJecsExecutorTestPeer;

class SubaruUnisiaJecsExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> TransportSetup(const FlashPlan& plan) const override;
    Result<FlashExecutionResult> Execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;

  private:
    friend class SubaruUnisiaJecsExecutorTestPeer;
    static Result<bytes::Bytes> ReadRange(std::uint32_t begin, std::uint32_t end, IKlineFlashTransport& transport,
                                          IClock& clock, const ICancellationToken& cancellation, IEventSink& events);
};
} // namespace fastecu::flash
