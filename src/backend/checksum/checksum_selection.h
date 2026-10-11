#pragma once
#include <optional>
#include <string>
#include "src/algorithms/checksum/checksum_result.h"
#include "src/algorithms/protocol/bytes.h"

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
        kUnknownMcuType,      // mcu_type not found in kFlashDevices[]
        kBadRomSize,          // rom size != kFlashDevices[index].romsize
        kNoModuleForProtocol, // make/checksum_flag/flash_method matched no family
        kFamilyRan,           // flash_method matched a family branch
    };
    Status status = Status::kNoModuleForProtocol;
    // Present iff a family's calculate_checksum_result actually ran. FamilyRan
    // itself can occur with family_result == std::nullopt for one legacy edge
    // case: flash_method matches "sub_ecu_hitachi_m32r_kline" but RomId's
    // leading digit is none of "3"/"4"/"6" -- module considered available (no
    // warning dialog), but no family runs and no bytes change. Its rom_data is
    // the corrected bytes of the ROM's ECU address range, not the ROM file.
    std::optional<ChecksumResult> family_result;
    // The ROM file after correction: present iff a family ran, its result is
    // Ok(), and every corrected byte went back through the memory map.
    std::optional<bytes::Bytes> corrected_file;
};

} // namespace fastecu::checksum
