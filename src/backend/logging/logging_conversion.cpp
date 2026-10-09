#include "src/backend/logging/logging_conversion.h"

#include "src/algorithms/expression/expression_evaluator.h"
#include "src/backend/logging/logging_session.h"
#include "src/backend/ports/error.h"

#include <charconv>
#include <cmath>
#include <string>

namespace fastecu::logging
{

fastecu::Result<LogSample> convert_sample(const LoggingSession& session, const ProtocolSample& raw)
{
    const LoggingChannel *channel = session.find_channel(raw.channel_id);
    if (channel == nullptr)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "protocol sample channel is not in the logging session");
    }

    // Legacy logging always evaluated intermediate expression results at 15
    // significant digits. Display precision is applied only by the UI adapter.
    constexpr int kCalculationPrecision = 15;
    double numeric_value = 0;
    if (channel->sample_bit.has_value())
    {
        unsigned byte = 0;
        const auto parsed = std::from_chars(raw.raw_value.data(), raw.raw_value.data() + raw.raw_value.size(), byte);
        if (parsed.ec != std::errc{} || parsed.ptr != raw.raw_value.data() + raw.raw_value.size() || byte > 255)
        {
            return fail(ErrorKind::BadResponse, "switch raw sample is not a byte");
        }
        numeric_value = static_cast<double>((byte >> *channel->sample_bit) & 1U);
    }
    else
    {
        numeric_value = expression_evaluate(channel->from_byte_expression, raw.raw_value, kCalculationPrecision);
    }
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

} // namespace fastecu::logging
