#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
Status CheckFamily(const FlashPlan& plan, FlashFamily expected_family)
{
    if (plan.Family() != expected_family)
    {
        return Fail(ErrorKind::kInvalidConfig, "plan family does not match this executor");
    }
    return {};
}

} // namespace fastecu::flash
