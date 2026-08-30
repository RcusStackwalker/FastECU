#include "src/ui/desktop-quick/dashboard/bundled_dashboard_loader.h"

#include "src/backend/dashboard/dashboard_session_builder.h"

namespace fastecu::desktop_quick
{

Result<dashboard::DashboardDocument> load_bundled_colt_dashboard(dashboard::DashboardDocumentService& service)
{
    auto document = service.load(kBundledColtDashboardHandle);
    if (!document.has_value())
    {
        return std::unexpected(document.error());
    }

    if (auto session = dashboard::prepare_dashboard_session(*document); !session.has_value())
    {
        return std::unexpected(session.error());
    }
    return document;
}

} // namespace fastecu::desktop_quick
