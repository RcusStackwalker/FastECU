#pragma once
#include <cstddef>
#include <functional>
#include <optional>

#include "src/backend/config/config_session.h"

// Asks the operator for a vehicle row; nullopt when they cancel.
using VehicleChooser = std::function<std::optional<std::size_t>()>;

// Runs after a successful composition and before MainWindow exists, which
// relies on a vehicle always being selected. With one selected it does
// nothing. Otherwise it asks `choose`: a chosen row is selected and saved,
// and startup continues (nullopt); a cancel returns the exit code main()
// ends with, before any window or ECU I/O exists. A failed save only means
// the next start asks again.
std::optional<int> startup_vehicle_gate(fastecu::config::ConfigSession& session, const VehicleChooser& choose);
