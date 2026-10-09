#include "src/backend/logging/logging_read_plan.h"

#include <format>

namespace fastecu::logging
{
Result<SsmReadPlan> make_ssm_read_plan(std::span<const LoggingChannel> channels)
{
    SsmReadPlan plan;
    for (const auto& channel : channels)
    {
        if (channel.length == 0 || channel.length > 84 || plan.addresses.size() > 84 - channel.length ||
            (!channel.byte_addresses.empty() && channel.byte_addresses.size() != channel.length) ||
            (channel.sample_bit.has_value() && (*channel.sample_bit >= 8 || channel.length != 1)))
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("SSM channel {}: invalid byte layout or exceeds 84 requested addresses", channel.id));
        }
        std::vector<std::size_t> positions;
        for (std::size_t i = 0; i < channel.length; ++i)
        {
            const auto address = channel.byte_addresses.empty() ? static_cast<std::uint64_t>(channel.address) + i
                                                                : channel.byte_addresses[i];
            if (address > 0xffffff)
            {
                return fail(ErrorKind::InvalidConfig,
                            std::format("SSM channel {}: byte address outside 24-bit range", channel.id));
            }
            positions.push_back(plan.addresses.size());
            plan.addresses.push_back(static_cast<std::uint32_t>(address));
        }
        plan.response_positions.push_back(std::move(positions));
    }
    return plan;
}
} // namespace fastecu::logging
