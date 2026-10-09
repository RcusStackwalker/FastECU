#include "apps/desktop/startup_vehicle_gate.h"

#include <cstdlib>

std::optional<int> startup_vehicle_gate(fastecu::config::ConfigSession& session, const VehicleChooser& choose,
                                        const SaveFailureReporter& report_save_failure)
{
    if (session.SelectedVehicle() != nullptr)
    {
        return std::nullopt;
    }
    const std::optional<std::size_t> row = choose();
    if (!row.has_value() || !session.SelectRow(*row).has_value())
    {
        return EXIT_SUCCESS;
    }
    if (const fastecu::Status saved = session.Save(); !saved.has_value())
    {
        report_save_failure(saved.error());
    }
    return std::nullopt;
}
