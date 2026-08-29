#pragma once

#include <string_view>

#include "src/backend/dashboard/dashboard_document_service.h"
#include "src/backend/ports/result.h"

namespace fastecu::desktop_quick
{

inline constexpr std::string_view kBundledColtDashboardHandle = ":/omnihaste/dashboards/colt-dashboard.ohd";

Result<dashboard::DashboardDocument> load_bundled_colt_dashboard(dashboard::DashboardDocumentService& service);

} // namespace fastecu::desktop_quick
