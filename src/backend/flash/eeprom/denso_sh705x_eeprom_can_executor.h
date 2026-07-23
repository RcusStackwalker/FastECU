#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Portable equivalent of the legacy EepromEcuSubaruDensoSH705xCanOperation
// (deleted by this task). Reproduces its exact wire bytes, chunk sizes,
// timeouts, and EEPROM-mode framing for the four DensoSecurityVariant values
// the portable plan can express (Stock/EcuTek/Cobb/EcuTekRaceRom); the
// confirm()/dialog loop moves to the desktop orchestrator (Task 17), same as
// the K-Line sibling (DensoSh705xEepromKlineExecutor).
//
// NOT implemented: the legacy "_ecutek_racerom_alt" flash-method branch
// (a temporary K-Line-shaped pair of RAM-location reads at 0xffff1ed8/
// 0xffff1e80 before resuming the CAN session, used to fold extra XOR
// material into the EcuTek seed key). This is a deliberate, explicit
// resolution of Task 4's OPEN QUESTION (denso_sh705x_eeprom_common.cpp) --
// see the long comment above connect_bootloader() in the .cpp for the full
// reasoning, and task-9-report.md for the writeup.
class DensoSh705xEepromCanExecutor final : public IFlashExecutor
{
  public:
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IFlashTransport& transport,
                                         IClock& clock, const ICancellationToken& cancellation,
                                         IEventSink& events) override;

  private:
    Status connect_bootloader(ICanFlashTransport& transport, IClock& clock,
                              const ICancellationToken& cancellation, IEventSink& events,
                              const DensoSh705xEepromCanPlan& can_plan, bool& kernel_alive);
    Status upload_kernel(ICanFlashTransport& transport, IClock& clock,
                         const ICancellationToken& cancellation, IEventSink& events,
                         const DensoSh705xEepromCanPlan& can_plan, const KernelImage& kernel);
    Result<bytes::Bytes> read_mem(ICanFlashTransport& transport, IClock& clock,
                                  const ICancellationToken& cancellation, IEventSink& events,
                                  const MemoryRegion& region, EepromReadMode mode,
                                  std::uint32_t request_id);
};

} // namespace fastecu::flash
