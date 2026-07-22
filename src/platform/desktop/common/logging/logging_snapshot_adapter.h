#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QString>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/logging/logging_session.h"
#include "src/backend/ports/result.h"

namespace fastecu::desktop::logging
{

// GUI-thread snapshot of the legacy parallel lists.  The session and map are
// passed to the worker as value data; they must not retain LogValuesStructure.
struct DesktopLoggingSnapshot
{
    const fastecu::logging::LoggingSession session;
    std::vector<std::size_t> response_offsets;
    std::unordered_map<std::string, int> index_by_id;
    std::unordered_set<std::string> enabled_ids;
};

fastecu::Result<DesktopLoggingSnapshot> make_desktop_logging_snapshot(
    const FileActions::LogValuesStructure& log_values,
    fastecu::logging::LoggingProtocolId protocol, const QString& protocol_filter,
    fastecu::logging::LoggingPolicy policy);

} // namespace fastecu::desktop::logging
