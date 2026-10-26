#pragma once

#include <chrono>

#include "src/backend/flash/ecu/denso_beef_can_profile.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/protocol/uds/uds_client.h"

namespace fastecu::flash
{

struct BeefKernelTransferPolicy
{
    uds::ExchangePolicy download_policy;
    std::chrono::milliseconds block_reply_timeout;
};

// The warm BEEF kernel's protocol operations. Collaborators and the profile
// remain alive in the caller; the kernel owns no transport lifecycle.
class BeefCanKernel
{
  public:
    BeefCanKernel(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                  IEventSink& events, uds::IUdsChannel& channel, const DensoBeefCanProfile& profile);

    Status TransferPayload(const KernelImage& kernel, const BeefKernelTransferPolicy& policy);
    Result<bytes::Bytes> Read(const MemoryRegion& region, PhaseReporter& progress);
    Status Write(const FlashPlan& plan, PhaseSequence& phases, PhaseReporter& compare_progress);

  private:
    ICanFlashTransport& transport_;
    const ICancellationToken& cancellation_;
    IEventSink& events_;
    uds::IUdsChannel& channel_;
    const DensoBeefCanProfile& profile_;
    uds::UdsClient uds_;
};

} // namespace fastecu::flash
