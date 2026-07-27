#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "src/backend/definition/definition_model.h"

namespace fastecu::definition
{

Result<std::vector<DefinitionIndexEntry>> parse_ecuflash_index(
    std::span<const std::uint8_t> xml, std::string_view source);

Result<UnresolvedDefinition> parse_ecuflash_definition(
    std::span<const std::uint8_t> xml, std::string_view source);

} // namespace fastecu::definition
