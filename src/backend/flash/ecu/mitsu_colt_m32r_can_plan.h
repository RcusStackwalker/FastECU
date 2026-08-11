#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

struct MitsuColtProtocolOptions
{
    bool use_vendor_challenge;
    std::uint32_t rom_size;
};

Result<MitsuColtProtocolOptions> parse_mitsu_colt_protocol(std::string_view protocol_name);

// Builds a Mitsubishi Colt CZT (Z37A, ROM 47110032) M32R CAN plan. Portable
// equivalent of the preflight half of FlashEcuMitsuM32rCanOperation, deleted
// by this wave.
//
// Owns no state and reads no files: unlike the EEPROM pair, this family
// loads no kernel and consults no catalog. Everything it needs is the
// selected protocol name, the MCU type string from the ROM definition, and
// (for a write) the ROM image. The protocol name selects both the vendor
// challenge and the ROM capacity.
//
// Validation runs here before any I/O: the exact supported protocol ID is
// checked first, followed by operation support, MCU lookup, and write-image
// capacity. That ordering is deliberate -- it is the FlashPlan contract, and
// it means an unsupported or misconfigured request never reaches the ECU.
//
// TestWrite is rejected as Unsupported. protocols.cfg declares
// test_write=no for every supported Mitsubishi Colt M32R CAN protocol,
// while the legacy class silently returned success after performing only the
// diagnostic-session handshake. Returning Unsupported follows the step-5c
// precedent set for the EEPROM write gap rather than legitimizing a no-op.
Result<FlashPlan> build_mitsu_colt_m32r_can_plan(FlashOperation operation,
                                                 std::string_view protocol_name,
                                                 std::string_view mcu_type,
                                                 std::optional<bytes::Bytes> image);

} // namespace fastecu::flash
