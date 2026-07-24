#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

Status check_family_transport_match(const FlashPlan& plan, FlashFamily expected_family,
                                    TransportKind expected_transport)
{
    if (plan.family() != expected_family)
    {
        return fail(ErrorKind::InvalidConfig, "plan family does not match this executor");
    }
    if (plan.transport() != expected_transport)
    {
        return fail(ErrorKind::InvalidConfig, "plan transport does not match this executor");
    }
    return {};
}

} // namespace fastecu::flash
