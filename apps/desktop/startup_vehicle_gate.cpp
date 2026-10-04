#include "apps/desktop/startup_vehicle_gate.h"

#include <cstdlib>
#include <tuple>

std::optional<int> startup_vehicle_gate(fastecu::config::ConfigSession& session, const VehicleChooser& choose)
{
    if (session.selected_vehicle() != nullptr)
    {
        return std::nullopt;
    }
    const std::optional<std::size_t> row = choose();
    if (!row.has_value() || !session.select_row(*row).has_value())
    {
        return EXIT_SUCCESS;
    }
    std::ignore = session.save();
    return std::nullopt;
}
