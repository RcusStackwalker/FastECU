#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/definition/definition_writer.h"
#include "src/backend/ports/result.h"

namespace fastecu::definition
{

using DefinitionHeaderFields = std::vector<std::pair<std::string, std::string>>;

// Input is UTF-8 text, regardless of any encoding declaration.
// Requested fields in order, with blank values for absent fields, parse failures or unsupported roots.
// Accept a rom root or the first direct rom child of a roms root.
// include and notes come from the ROM; other fields come from its romid.
// Uses non-validating pugixml parsing; custom DTD entities are not expanded.
DefinitionHeaderFields collect_ecuflash_base_header_fields(std::span<const std::string> names, std::string_view xml);

// Last value for each name wins. Only xmlid and internalidaddress are trimmed.
// An empty address is absent; invalid/overflowing hexadecimal addresses fail.
Result<DefinitionHeaderInput> definition_header_input(std::span<const std::pair<std::string, std::string>> fields);

} // namespace fastecu::definition
