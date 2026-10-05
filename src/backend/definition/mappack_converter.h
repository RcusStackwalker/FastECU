#pragma once

#include <string>
#include <string_view>
#include "src/backend/ports/result.h"

namespace fastecu::definition
{
// Converts a complete semicolon-separated MapPack CSV to RomRaider XML.
// Pure: no file I/O. Invalid input returns InvalidConfig with row/column context.
Result<std::string> convert_mappack_csv(std::string_view csv, std::string_view ecu_id);
} // namespace fastecu::definition
