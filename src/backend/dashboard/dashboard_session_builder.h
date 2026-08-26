#pragma once

#include <utility>

#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/logging/logging_session.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/cdbg_protocol_config.h"

namespace fastecu::dashboard
{
class PreparedDashboardSession
{
  public:
    PreparedDashboardSession(const PreparedDashboardSession&) = delete;
    PreparedDashboardSession& operator=(const PreparedDashboardSession&) = delete;
    PreparedDashboardSession(PreparedDashboardSession&&) = default;
    PreparedDashboardSession& operator=(PreparedDashboardSession&&) = default;

    const logging::LoggingSession& session() const;
    const cdbg::CdbgProtocolConfig& config() const;
    std::pair<logging::LoggingSession, cdbg::CdbgProtocolConfig> into_parts() &&;

  private:
    PreparedDashboardSession(logging::LoggingSession session, cdbg::CdbgProtocolConfig config);

    logging::LoggingSession session_;
    cdbg::CdbgProtocolConfig config_;

    friend fastecu::Result<PreparedDashboardSession> prepare_dashboard_session(const DashboardDocument& document);
};

fastecu::Result<PreparedDashboardSession> prepare_dashboard_session(const DashboardDocument& document);
} // namespace fastecu::dashboard
