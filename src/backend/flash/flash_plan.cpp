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
    }
    return "Unknown";
}

} // namespace fastecu::flash
