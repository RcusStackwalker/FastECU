#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "src/backend/definition/definition_writer.h"
#include "src/backend/ports/result.h"

namespace fastecu::definition
{

// An editable header. Address text may be incomplete until submission.
struct DefinitionHeaderDraft
{
    std::string xml_id;
    std::string internal_id;
    std::string ecu_id;
    std::string internal_id_address_text;
    RomMetadata metadata;
    std::string include;
    std::string notes;
};

// Read a partial rom header or the first direct rom inside roms, without parsing tables.
// Scalar values are normalized; notes and editable address spelling are preserved.
// Source bytes use XML encoding autodetection; decoded text is explicitly UTF-8.
Result<DefinitionHeaderDraft> ReadDefinitionHeader(std::span<const std::uint8_t> xml);
Result<DefinitionHeaderDraft> ReadDefinitionHeader(std::string_view xml);

// Convert a draft for submission: blank addresses are absent, invalid uint64 hex fails.
Result<DefinitionHeaderInput> BuildDefinitionHeaderInput(const DefinitionHeaderDraft& draft);

} // namespace fastecu::definition
