#include "src/backend/logging/logging_conversion.h"

#include "src/algorithms/expression/checked_expression.h"
#include "src/backend/logging/logging_session.h"
#include "src/backend/ports/error.h"

#include <cmath>
#include <string>

namespace fastecu::logging
{

fastecu::Result<LogSample> ConvertSample(const LoggingSession& session, const ProtocolSample& raw)
{
    const LoggingChannel *channel = session.FindChannel(raw.channel_id);
    if (channel == nullptr)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "protocol sample channel is not in the logging session");
    }

    const auto raw_value = fastecu::expression::ParseFiniteNumber(raw.raw_value);
    if (!raw_value.has_value())
    {
        return fastecu::Fail(fastecu::ErrorKind::kBadResponse,
                             "protocol sample raw value is not a finite number: " + raw_value.error().detail);
    }
    const auto numeric_value = fastecu::expression::EvaluateChecked(channel->from_byte_expression, *raw_value);
    if (!numeric_value.has_value())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig,
                             "logging conversion expression failed: " + numeric_value.error().detail);
    }

    return LogSample{
        .channel_id = raw.channel_id,
        .numeric_value = *numeric_value,
        .raw_value = raw.raw_value,
        .unit = channel->unit,
    };
}

} // namespace fastecu::logging
