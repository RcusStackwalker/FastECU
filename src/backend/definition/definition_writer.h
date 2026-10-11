#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "src/algorithms/memory/address.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::definition
{

struct DefinitionHeaderInput
{
    std::string xml_id;
    std::string internal_id;
    std::string ecu_id;
    std::optional<memory::DefinitionAddress> internal_id_address;
    RomMetadata metadata;
    std::string include;
    std::string notes;
};

// Normalize scalar header values; preserve internal spaces and both notes fields.
DefinitionHeaderInput NormalizeHeaderInput(DefinitionHeaderInput input);

Result<std::vector<std::uint8_t>> CreateEcuflashXml(const DefinitionHeaderInput&);
Result<std::vector<std::uint8_t>> RewriteEcuflashXml(std::span<const std::uint8_t> source,
                                                     const DefinitionHeaderInput&);

} // namespace fastecu::definition
