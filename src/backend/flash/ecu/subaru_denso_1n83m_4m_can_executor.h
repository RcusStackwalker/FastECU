#pragma once
#include "src/backend/flash/ecu/denso_iso15765_bootloader_executor.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"

namespace fastecu::flash
{
extern template class DensoIso15765BootloaderExecutor<SubaruDenso1n83m_4mCanPlan>;
using SubaruDenso1n83m_4mCanExecutor = DensoIso15765BootloaderExecutor<SubaruDenso1n83m_4mCanPlan>;
} // namespace fastecu::flash
