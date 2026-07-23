#pragma once

#include <QString>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"

namespace fastecu::desktop::logging
{

QString format_logging_value(double value, int precision);

fastecu::Status apply_log_sample(const DesktopLoggingSnapshot& snapshot,
                                 const fastecu::logging::LogSample& sample,
                                 FileActions::LogValuesStructure& log_values);

} // namespace fastecu::desktop::logging
