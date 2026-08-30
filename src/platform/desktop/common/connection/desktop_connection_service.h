#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/dashboard/dashboard_session_builder.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/connection/local_adapter.h"
#include "src/platform/desktop/common/logging/logging_engine.h"

namespace fastecu::desktop::connection
{

struct PreparedConnection
{
    logging::LoggingRun run;
    LocalAdapterDescriptor selected;
};

struct AdapterSelectionRequired
{
    AdapterDiscoverySnapshot snapshot;
    enum class Reason
    {
        NoPreference,
        NoMatch,
        AmbiguousMatch,
        StaleSelection,
    } reason;
};

using ConnectionPreparationOutcome = std::variant<PreparedConnection, AdapterSelectionRequired, Error>;

struct AdapterSelection
{
    std::uint64_t generation;
    std::string candidate_id;
};

class DesktopConnectionService
{
  public:
    explicit DesktopConnectionService(std::vector<ILocalAdapterProvider *> providers);

    Result<AdapterDiscoverySnapshot> refresh();
    ConnectionPreparationOutcome prepare_run(const dashboard::DashboardDocument& document,
                                             std::optional<AdapterSelection> selection = std::nullopt);

  private:
    ConnectionPreparationOutcome open_run(dashboard::PreparedDashboardSession prepared_session,
                                          const dashboard::CdbgConnectionProfile& profile,
                                          const LocalAdapterDescriptor& descriptor, ILocalAdapterProvider& provider);

    std::vector<ILocalAdapterProvider *> providers_;
    std::uint64_t generation_ = 0;
    std::optional<AdapterDiscoverySnapshot> current_snapshot_;
    std::unordered_map<std::string, ILocalAdapterProvider *> candidate_providers_;
};

} // namespace fastecu::desktop::connection
