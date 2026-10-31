#pragma once

#include "src/backend/flash/ecu/subaru_unisia_jecs_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_types.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruUnisiaJecsExecutorTestPeer;

class SubaruUnisiaJecsExecutor final : public FamilyExecutor<SubaruUnisiaJecsPlan, IKlineFlashExecutor>
{
  private:
    friend class SubaruUnisiaJecsExecutorTestPeer;
    Result<KlineConfig> Setup(const SubaruUnisiaJecsPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruUnisiaJecsPlan& family, const FlashPlan& plan,
                                     IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
    static Result<bytes::Bytes> ReadRange(std::uint32_t begin, std::uint32_t end, IKlineFlashTransport& transport,
                                          IClock& clock, const ICancellationToken& cancellation, IEventSink& events);
};
} // namespace fastecu::flash
