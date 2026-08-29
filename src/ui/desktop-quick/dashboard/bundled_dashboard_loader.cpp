#include "src/ui/desktop-quick/dashboard/bundled_dashboard_loader.h"

#include <string>

#include "src/backend/dashboard/dashboard_session_builder.h"

namespace fastecu::desktop_quick
{

Result<dashboard::DashboardDocument> load_bundled_colt_dashboard(dashboard::DashboardDocumentService& service)
{
    auto document = service.load(kBundledColtDashboardHandle);
    if (!document)
    {
        return std::unexpected(document.error());
    }

    for (const dashboard::DashboardCard& card : document->cards)
    {
        if (card.display_type != dashboard::CardDisplayType::Numeric)
        {
            return fail(ErrorKind::Unsupported,
                        "cards[" + card.id + "].display-type: only numeric cards are supported");
        }
    }

    if (auto session = dashboard::prepare_dashboard_session(*document); !session)
    {
        return std::unexpected(session.error());
    }
    return document;
}

} // namespace fastecu::desktop_quick
