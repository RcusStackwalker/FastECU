#include "src/backend/logging/logging_sample_resolution.h"

#include <format>

#include "src/backend/logging/logging_run_snapshot.h"

namespace fastecu::logging
{
fastecu::Result<std::optional<ResolvedLogSample>> resolve_log_sample(const LoggingRunSnapshot& snapshot,
                                                                     const LogSample& sample)
{
    const auto *channel = snapshot.session().find_channel(sample.channel_id);
    if (channel == nullptr)
    {
        return fastecu::fail(ErrorKind::Internal,
                             std::format("{} sample {}: logging sample id is not in the run snapshot",
                                         snapshot.protocol_key(), sample.channel_id));
    }
    const LoggingMeasurement *measurement = nullptr;
    for (const auto& item : snapshot.measurements())
    {
        if (item.channel_id == sample.channel_id)
        {
            measurement = &item;
            break;
        }
    }
    if (measurement == nullptr)
    {
        return fail(ErrorKind::Internal, "logging channel has no captured measurement identity");
    }
    return ResolvedLogSample{
        .kind = measurement->kind,
        .identity = measurement->identity,
        .numeric_value = sample.numeric_value,
        .decimal_precision = channel->decimal_precision,
    };
}
} // namespace fastecu::logging
