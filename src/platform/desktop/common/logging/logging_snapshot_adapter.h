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

// GUI-thread-owned state needed to apply portable samples to legacy parallel
// lists. It deliberately owns no portable logging session.
struct LegacyLoggingMapping
{
    std::vector<std::size_t> response_offsets;
    std::unordered_map<std::string, int> index_by_id;
    std::unordered_set<std::string> enabled_ids;
};

// Bundles the one portable session with legacy-only mapping state until their
// owners split them for the logging engine and GUI coordinator respectively.
struct PreparedLegacyLoggingSession
{
    fastecu::logging::LoggingSession session;
    LegacyLoggingMapping mapping;
};

fastecu::Result<PreparedLegacyLoggingSession>
make_prepared_legacy_logging_session(const FileActions::LogValuesStructure& log_values,
                                     fastecu::logging::LoggingProtocolId protocol, const QString& protocol_filter,
                                     fastecu::logging::LoggingPolicy policy);

} // namespace fastecu::desktop::logging
