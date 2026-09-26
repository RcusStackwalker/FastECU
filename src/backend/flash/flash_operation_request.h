#pragma once

#include <string>
#include <string_view>

#include "src/backend/flash/flash_types.h"

namespace fastecu::flash
{

// The desktop menu's command strings: "write" and "test_write" write;
// anything else, including "read", reads.
FlashOperation flash_operation_from_command(std::string_view command);

// The two Denso TCU CAN protocols whose reads first offer service functions.
// Exact match: suffixed future protocols are not TCU reads.
bool is_denso_tcu_protocol(std::string_view protocol);

// dir + file, inserting '/' when a non-empty dir does not end with one.
std::string kernel_path(std::string_view dir, std::string_view file);

// "<rom_id><timestamp>.bin", or "read_image_<timestamp>.bin" when rom_id is empty.
std::string read_image_filename(std::string_view rom_id, std::string_view timestamp);

} // namespace fastecu::flash
