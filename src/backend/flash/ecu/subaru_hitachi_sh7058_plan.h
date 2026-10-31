#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> BuildSubaruHitachiSh7058Plan(FlashOperation operation, std::string_view protocol,
                                               std::string_view mcu, std::optional<bytes::Bytes> image);
Status ValidateSubaruHitachiSh7058Plan(const FlashPlan& plan);

// The two transports share one plan validator and one builder. Each executor
// supports one operation, so the mode guard sits in its own specialization and
// the shared validator keeps accepting both.
template <> struct PlanValidator<SubaruHitachiSh7058CanPlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        if (auto valid = ValidateSubaruHitachiSh7058Plan(plan); !valid.has_value())
        {
            return valid;
        }
        return plan.Operation() == FlashOperation::kWrite
                   ? Status{}
                   : Fail(ErrorKind::kUnsupported, "SH7058 CAN supports write only");
    }
};

template <> struct PlanValidator<SubaruHitachiSh7058KlinePlan>
{
    static Status Validate(const FlashPlan& plan)
    {
        if (auto valid = ValidateSubaruHitachiSh7058Plan(plan); !valid.has_value())
        {
            return valid;
        }
        return plan.Operation() == FlashOperation::kRead
                   ? Status{}
                   : Fail(ErrorKind::kUnsupported, "SH7058 K-Line supports read only");
    }
};
// Exact protocol ids this family accepts, in table order.
std::span<const std::string_view> SubaruHitachiSh7058Protocols();
} // namespace fastecu::flash
