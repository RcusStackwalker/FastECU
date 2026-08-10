#pragma once
#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Builds a Mitsubishi Colt CZT (Z37A, ROM 47110032) M32R CAN plan. Portable
// equivalent of the preflight half of FlashEcuMitsuM32rCanOperation, deleted
// by this wave.
//
// Owns no state and reads no files: unlike the EEPROM pair, this family
// loads no kernel and consults no catalog. Everything it needs is the
// selected protocol name, the MCU type string from the ROM definition, the
// vendor-challenge flag MainWindow already passes to the dialog, and (for a
// write) the ROM image.
//
// Three checks the legacy class performed AFTER opening the port and
// completing the bootloader handshake run here instead, before any I/O:
// unknown MCU type, a write image not exactly kFullRomSize, and an unsupported
// operation. That ordering change is deliberate -- it is the FlashPlan
// contract, and it means a misconfigured write never reaches the ECU -- and
// is recorded in the flash qualification matrix.
//
// TestWrite is rejected as Unsupported. protocols.cfg declares
// test_write=no for both mitsu_ecu_m32r_can and mitsu_ecu_m32r_can_vendor_ext,
// while the legacy class silently returned success after performing only the
// diagnostic-session handshake. Returning Unsupported follows the step-5c
// precedent set for the EEPROM write gap rather than legitimizing a no-op.
Result<FlashPlan> build_mitsu_colt_m32r_can_plan(FlashOperation operation,
                                                 std::string_view protocol_name,
                                                 std::string_view mcu_type,
                                                 bool use_vendor_challenge,
                                                 std::optional<bytes::Bytes> image);

} // namespace fastecu::flash
