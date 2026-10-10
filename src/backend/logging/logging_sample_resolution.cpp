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
    if (!snapshot.ChannelEnabled(sample.channel_id))
    {
        return std::nullopt;
    }
    return ResolvedLogSample{
        .identity = {snapshot.ProtocolKey(), channel->id},
        .numeric_value = sample.numeric_value,
        .decimal_precision = channel->decimal_precision,
    };
}
} // namespace fastecu::logging
