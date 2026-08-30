#pragma once

#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/ports/result.h"

namespace fastecu::dashboard
{
Status validate_dashboard_document(const DashboardDocument& document);
} // namespace fastecu::dashboard
