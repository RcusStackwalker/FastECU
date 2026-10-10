#include "src/backend/logging/logging_sample_resolution.h"

#include <format>

#include "src/backend/logging/logging_run_snapshot.h"

namespace fastecu::logging
{
fastecu::Result<std::optional<ResolvedLogSample>> ResolveLogSample(const LoggingRunSnapshot& snapshot,
                                                                   const LogSample& sample)
{
    const auto *channel = snapshot.Session().FindChannel(sample.channel_id);
    if (channel == nullptr)
    {
        return fastecu::Fail(ErrorKind::kInternal,
                             std::format("{} sample {}: logging sample id is not in the run snapshot",
                                         snapshot.ProtocolKey(), sample.channel_id));
    }
    const LoggingMeasurement *measurement = nullptr;
    for (const auto& item : snapshot.Measurements())
    {
        if (item.channel_id == sample.channel_id)
        {
            measurement = &item;
            break;
        }
    }
    if (measurement == nullptr)
    {
        return Fail(ErrorKind::kInternal, "logging channel has no captured measurement identity");
    }
    return ResolvedLogSample{
        .kind = measurement->kind,
        .identity = measurement->identity,
        .numeric_value = sample.numeric_value,
        .decimal_precision = channel->decimal_precision,
    };
}
} // namespace fastecu::logging
