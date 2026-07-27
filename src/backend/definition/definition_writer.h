#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::definition
{

struct DefinitionHeaderInput
{
    std::string xml_id, internal_id, ecu_id;
    std::uint64_t internal_id_address;
    RomMetadata metadata;
    std::string include, notes;
};

Result<std::vector<std::uint8_t>> create_ecuflash_xml(const DefinitionHeaderInput&);
Result<std::vector<std::uint8_t>> rewrite_ecuflash_xml(
    std::span<const std::uint8_t> source,
    const DefinitionHeaderInput&);

} // namespace fastecu::definition
