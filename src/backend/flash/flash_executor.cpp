#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
Status check_family(const FlashPlan& plan, FlashFamily expected_family)
{
    if (plan.family() != expected_family)
    {
        return fail(ErrorKind::InvalidConfig, "plan family does not match this executor");
    }
    return {};
}

} // namespace fastecu::flash
