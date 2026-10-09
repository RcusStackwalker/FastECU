#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "src/backend/definition/definition_model.h"

namespace fastecu::definition
{

Result<std::vector<DefinitionIndexEntry>> ParseEcuflashIndex(std::span<const std::uint8_t> xml,
                                                             std::string_view source);

Result<UnresolvedDefinition> ParseEcuflashDefinition(std::span<const std::uint8_t> xml, std::string_view source);

} // namespace fastecu::definition
