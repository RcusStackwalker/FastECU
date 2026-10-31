// src/backend/flash/eeprom/eeprom_read_plan.h
#pragma once

#include <span>
#include <string_view>

#include "src/backend/config/catalog.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Builds a Denso SH705x EEPROM read plan of `family` (kDensoSh705xEepromKline
// or kDensoSh705xEepromCan, as the flash route chose it) for `protocol`, the
// selected vehicle's protocol. Any other family is kInvalidConfig. Owns no state; the returned FlashPlan owns its
// kernel bytes by value.
//
// The caller resolves the protocol from the selected vehicle, so only a
// vehicle-backed protocol ever reaches here -- the guarantee the former
// car-model lookup gave, now carried by the catalog's invariant that every
// protocol has a vehicle.
//
// Every fallible validation decidable from the protocol runs before the
// kernel read, so an invalid mode, security variant, MCU/region, or missing
// or definitely out-of-range kernel address is rejected without reading the
// kernel file. Validation that needs the kernel byte count runs after the
// read. This ordering is a guarantee, not an accident -- the tests assert it.
Result<FlashPlan> BuildEepromReadPlan(FlashFamily family, const config::ConfigPaths& paths,
                                      const config::ProtocolSpec& protocol, EepromReadMode mode,
                                      IFileRepository& file_repository);

// Exact protocol ids routed to each EEPROM family, in table order. The
// builder takes its family from the route; it reads only the security
// suffix from the protocol name and does not check these lists.
std::span<const std::string_view> DensoSh705xEepromKlineProtocols();
std::span<const std::string_view> DensoSh705xEepromCanProtocols();

} // namespace fastecu::flash
