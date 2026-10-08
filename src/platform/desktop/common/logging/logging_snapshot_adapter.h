#pragma once

#include <QString>

#include "src/backend/logging/logging_run_snapshot.h"

namespace fastecu::desktop::logging
{
using DesktopLoggingSnapshot = fastecu::logging::LoggingRunSnapshot;

fastecu::Result<DesktopLoggingSnapshot> make_desktop_logging_snapshot(const fastecu::logging::LoggerModel& model,
                                                                      fastecu::logging::LoggingProtocolId protocol,
                                                                      const QString& protocol_filter,
                                                                      fastecu::logging::LoggingPolicy policy,
                                                                      fastecu::logging::LoggingTarget target);
} // namespace fastecu::desktop::logging
