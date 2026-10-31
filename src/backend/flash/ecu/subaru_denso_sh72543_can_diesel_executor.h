#pragma once
#include "src/backend/flash/ecu/denso_iso15765_bootloader_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"

namespace fastecu::flash
{
extern template class DensoIso15765BootloaderExecutor<SubaruDensoSh72543CanDieselPlan>;
using SubaruDensoSh72543CanDieselExecutor = DensoIso15765BootloaderExecutor<SubaruDensoSh72543CanDieselPlan>;
} // namespace fastecu::flash
