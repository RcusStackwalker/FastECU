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

Result<ICanFlashTransport *> open_can_iso15765_transport(IFlashTransport& transport, const Iso15765Config& config)
{
    auto *can = dynamic_cast<ICanFlashTransport *>(&transport);
    if (can == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "transport does not implement ICanFlashTransport");
    }
    if (const Status configured = can->configure(config); !configured.has_value())
    {
        return std::unexpected(configured.error());
    }
    if (const Status opened = can->open(); !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    return can;
}

} // namespace fastecu::flash
