// src/backend/flash/eeprom/eeprom_read_plan.h
#pragma once
#include <string_view>

#include "src/backend/config/config_paths.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Builds a Denso SH705x EEPROM read plan for `protocol_name`, replacing
// LegacyFlashSnapshotAdapter (step 5d-6). Owns no state: every catalog it
// loads is a local, and the returned FlashPlan owns its kernel bytes by
// value.
//
// Resolution goes through the car-model catalog -- load_protocol_catalog +
// load_car_model_catalog + resolve_car_models +
// find_car_model_by_protocol_name -- and NOT through a direct scan of
// ProtocolCatalog, deliberately. Legacy's selection UIs index
// car-model-derived lists, so a protocol referenced by no <car_model> can
// never be the selected one; a direct scan would accept exactly those names,
// widening the accepted input set on the path that writes to an ECU. It also
// keeps this in lockstep with LegacyCalibrationAdapter::bind_protocol, which
// uses the same pair, so the two can never disagree about which
// ProtocolEntry a protocol name denotes.
//
// An unresolved protocol is a HARD ERROR here (ErrorKind::InvalidConfig).
// This diverges deliberately from bind_protocol, which substitutes a
// single-space placeholder for a protocol-derived field: a placeholder MCU
// or kernel address would build a plan that flashes garbage to an ECU. Do
// not "make this consistent" with the calibration adapter.
//
// Every fallible validation decidable from catalog metadata runs before the
// kernel read, so an invalid mode, security variant, MCU/region, or definitely
// out-of-range kernel address is rejected without reading the kernel file.
// Validation that requires the kernel byte count runs after the read. This
// ordering is a guarantee, not an accident -- the tests assert it.
Result<FlashPlan> build_eeprom_read_plan(const config::ConfigPaths& paths,
                                         std::string_view protocol_name,
                                         EepromReadMode mode,
                                         IFileRepository& file_repository);

} // namespace fastecu::flash
