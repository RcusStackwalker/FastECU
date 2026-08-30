#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

std::string_view FlashPlan::experimental_family_id() const
{
    switch (fields_.family)
    {
    case FlashFamily::DensoSh705xEepromKline:
        return "DensoSh705xEepromKline";
    case FlashFamily::DensoSh705xEepromCan:
        return "DensoSh705xEepromCan";
    case FlashFamily::MitsuColtM32rCan:
        return "MitsuColtM32rCan";
    case FlashFamily::SubaruMitsuM32rKline:
        return "SubaruMitsuM32rKline";
    case FlashFamily::SubaruHitachiM32rKline:
        return "SubaruHitachiM32rKline";
    case FlashFamily::SubaruDensoMc68hc16y5_02:
        return "SubaruDensoMc68hc16y5_02";
    case FlashFamily::SubaruDensoSh7055_02:
        return "SubaruDensoSh7055_02";
    case FlashFamily::SubaruHitachiM32rCan:
        return "SubaruHitachiM32rCan";
    case FlashFamily::SubaruTcuCvtHitachiM32rCan:
        return "SubaruTcuCvtHitachiM32rCan";
    case FlashFamily::SubaruTcuCvtMitsuMh8111Can:
        return "SubaruTcuCvtMitsuMh8111Can";
    case FlashFamily::SubaruTcuCvtMitsuMh8104Can:
        return "SubaruTcuCvtMitsuMh8104Can";
    case FlashFamily::SubaruDenso1n83m_1_5mCan:
        return "SubaruDenso1n83m_1_5mCan";
    case FlashFamily::SubaruDensoSh72531Can:
        return "SubaruDensoSh72531Can";
    }
    return "Unknown";
}

} // namespace fastecu::flash
