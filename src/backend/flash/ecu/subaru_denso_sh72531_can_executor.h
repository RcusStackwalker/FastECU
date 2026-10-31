#pragma once
#include "src/backend/flash/ecu/denso_iso15765_bootloader_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"

namespace fastecu::flash
{
extern template class DensoIso15765BootloaderExecutor<SubaruDensoSh72531CanPlan>;
using SubaruDensoSh72531CanExecutor = DensoIso15765BootloaderExecutor<SubaruDensoSh72531CanPlan>;
} // namespace fastecu::flash
