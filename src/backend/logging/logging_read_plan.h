#pragma once

#include <span>
#include <vector>

#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{
struct SsmReadPlan
{
    std::vector<std::uint32_t> addresses;
    std::vector<std::vector<std::size_t>> response_positions;
};
Result<SsmReadPlan> make_ssm_read_plan(std::span<const LoggingChannel> channels);
} // namespace fastecu::logging
