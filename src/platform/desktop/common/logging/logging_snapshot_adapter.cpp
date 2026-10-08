#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"

namespace fastecu::desktop::logging
{
fastecu::Result<DesktopLoggingSnapshot> make_desktop_logging_snapshot(const fastecu::logging::LoggerModel& model,
                                                                      fastecu::logging::LoggingProtocolId protocol,
                                                                      const QString& protocol_filter,
                                                                      fastecu::logging::LoggingPolicy policy,
                                                                      fastecu::logging::LoggingTarget target)
{
    return fastecu::logging::prepare_logging_run(model, protocol, protocol_filter.toStdString(), policy, target);
}
} // namespace fastecu::desktop::logging
