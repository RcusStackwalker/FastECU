#pragma once
#include <optional>
#include <string>
#include "src/algorithms/checksum/checksum_result.h"

namespace fastecu::checksum
{

struct ChecksumSelection
{
    std::string make;          // the selected vehicle's make (ConfigSession)
    std::string checksum_flag; // the selected vehicle's checksum flag: "yes"/"no"/"n/a" verbatim
    std::string flash_method;  // the selected vehicle's protocol_name
    std::string mcu_type;      // calibration session protocol MCU
    std::string rom_id;        // calibration session protocol ROM ID
};

struct ChecksumCorrectionOutcome
{
    enum class Status
    {
        UnknownMcuType,      // mcu_type not found in flashdevices[]
        BadRomSize,          // rom size != flashdevices[index].romsize
        NoModuleForProtocol, // make/checksum_flag/flash_method matched no family
        FamilyRan,           // flash_method matched a family branch
    };
    Status status = Status::NoModuleForProtocol;
    // Present iff a family's calculate_checksum_result actually ran. FamilyRan
    // itself can occur with family_result == std::nullopt for one legacy edge
    // case: flash_method matches "sub_ecu_hitachi_m32r_kline" but RomId's
    // leading digit is none of "3"/"4"/"6" -- module considered available (no
    // warning dialog), but no family runs and no bytes change.
    std::optional<ChecksumResult> family_result;
};

} // namespace fastecu::checksum
