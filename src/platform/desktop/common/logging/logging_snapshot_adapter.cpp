#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"

namespace fastecu::desktop::logging
{
fastecu::Result<DesktopLoggingSnapshot> MakeDesktopLoggingSnapshot(const fastecu::logging::LoggerModel& model,
                                                                   fastecu::logging::LoggingProtocolId protocol,
                                                                   const QString& protocol_filter,
                                                                   fastecu::logging::LoggingPolicy policy,
                                                                   fastecu::logging::LoggingTarget target)
{
    return fastecu::logging::PrepareLoggingRun(model, protocol, protocol_filter.toStdString(), policy, target);
}
} // namespace fastecu::desktop::logging
