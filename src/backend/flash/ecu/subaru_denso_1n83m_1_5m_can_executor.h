#pragma once
#include "src/backend/flash/ecu/denso_iso15765_bootloader_executor.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"

namespace fastecu::flash
{
extern template class DensoIso15765BootloaderExecutor<SubaruDenso1n83m_1_5mCanPlan>;
using SubaruDenso1n83m_1_5mCanExecutor = DensoIso15765BootloaderExecutor<SubaruDenso1n83m_1_5mCanPlan>;
} // namespace fastecu::flash
