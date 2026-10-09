#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QString>

#include "src/backend/logging/logger_model.h"
#include "src/backend/logging/logging_session.h"
#include "src/backend/ports/result.h"

namespace fastecu::desktop::logging
{

// GUI-thread snapshot. Workers receive owned values, never model references.
struct DesktopLoggingSnapshot
{
    fastecu::logging::LoggingSession session;
    std::vector<std::size_t> response_offsets;
    std::string protocol;
    fastecu::logging::LoggerSelection selection;
    std::unordered_map<std::string, fastecu::logging::LoggerIdentity> identities_by_id;
    std::unordered_set<std::string> enabled_ids;
    bool target_is_ecu = true;
};

fastecu::Result<DesktopLoggingSnapshot> MakeDesktopLoggingSnapshot(const fastecu::logging::LoggerModel& model,
                                                                   fastecu::logging::LoggingProtocolId protocol,
                                                                   const QString& protocol_filter,
                                                                   fastecu::logging::LoggingPolicy policy);

} // namespace fastecu::desktop::logging
