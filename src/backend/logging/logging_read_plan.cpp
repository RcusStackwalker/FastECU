#include "src/backend/logging/logging_read_plan.h"

#include <format>

namespace fastecu::logging
{
Result<SsmReadPlan> MakeSsmReadPlan(std::span<const LoggingChannel> channels)
{
    SsmReadPlan plan;
    for (const auto& channel : channels)
    {
        // A8, mode and three bytes per address must fit a one-byte body length.
        if (channel.length == 0 || channel.length > 84 || plan.addresses.size() > 84 - channel.length)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("SSM channel {}: exceeds 84 requested byte addresses", channel.id));
        }
        if (channel.address > 0xffffff || channel.length - 1 > 0xffffff - channel.address)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("SSM channel {}: byte address outside 24-bit range", channel.id));
        }
        plan.response_offsets.push_back(plan.addresses.size());
        for (std::size_t i = 0; i < channel.length; ++i)
        {
            plan.addresses.push_back(channel.address + static_cast<std::uint32_t>(i));
        }
    }
    return plan;
}
} // namespace fastecu::logging
