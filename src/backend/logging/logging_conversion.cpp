#include "src/backend/logging/logging_conversion.h"

#include <cmath>

#include "src/algorithms/expression/expression_evaluator.h"

fastecu::Result<LogSample> convert_sample(const LoggingSession& session,
                                          const ProtocolSample& raw)
{
    const LoggingChannel *channel = session.find_channel(raw.channel_id);
    if (channel == nullptr)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal,
                             "protocol sample channel is not in the logging session");
    }

    const double numeric_value = expression_evaluate(
        channel->from_byte_expression, raw.raw_value,
        static_cast<int>(channel->decimal_precision));
    if (!std::isfinite(numeric_value))
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "protocol sample evaluates to a non-finite logging value");
    }

    return LogSample{
        .channel_id = raw.channel_id,
        .numeric_value = numeric_value,
        .raw_value = raw.raw_value,
        .unit = channel->unit,
    };
}
