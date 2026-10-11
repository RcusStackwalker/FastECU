#include "src/backend/definition/definition_header_fields.h"

#include <utility>

#include <pugixml.hpp>

#include "src/backend/definition/parser_utils.h"
#include "src/backend/definition/metadata_fields.h"

namespace fastecu::definition
{
namespace
{
Result<DefinitionHeaderDraft> ReadHeader(std::span<const std::uint8_t> bytes, pugi::xml_encoding encoding)
{
    pugi::xml_document document;
    const auto parsed = ParseDocumentRoot(document, bytes, "authoring header", encoding);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    const std::string_view root_name{parsed->name()};
    const auto rom = root_name == "rom" ? *parsed : root_name == "roms" ? parsed->child("rom") : pugi::xml_node{};
    if (!rom)
    {
        return Invalid("authoring header", "XML document", "expected <rom> or a direct <rom> child of <roms>");
    }
    if (auto status = ValidateHeaderStructure(rom, "authoring header"); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    const auto rom_id = rom.child("romid");
    RomMetadata metadata;
    for (const auto& field : kEditableMetadataFields)
    {
        metadata.*field.member = HeaderChildText(rom_id, field.xml_name);
    }
    return DefinitionHeaderDraft{.xml_id = HeaderChildText(rom_id, "xmlid"),
                                 .internal_id = HeaderChildText(rom_id, "internalidstring"),
                                 .ecu_id = HeaderChildText(rom_id, "ecuid"),
                                 .internal_id_address_text = HeaderChildText(rom_id, "internalidaddress"),
                                 .metadata = std::move(metadata),
                                 .include = HeaderChildText(rom, "include"),
                                 .notes = HeaderChildText(rom, "notes")};
}
} // namespace

Result<DefinitionHeaderDraft> ReadDefinitionHeader(std::span<const std::uint8_t> xml)
{
    return ReadHeader(xml, pugi::encoding_auto);
}

Result<DefinitionHeaderDraft> ReadDefinitionHeader(std::string_view xml)
{
    return ReadHeader({reinterpret_cast<const std::uint8_t *>(xml.data()), xml.size()}, pugi::encoding_utf8);
}

Result<DefinitionHeaderInput> BuildDefinitionHeaderInput(const DefinitionHeaderDraft& draft)
{
    const auto address = ParseHeaderAddress(draft.internal_id_address_text, "authoring header");
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    return NormalizeHeaderInput(DefinitionHeaderInput{.xml_id = draft.xml_id,
                                                      .internal_id = draft.internal_id,
                                                      .ecu_id = draft.ecu_id,
                                                      .internal_id_address = *address,
                                                      .metadata = draft.metadata,
                                                      .include = draft.include,
                                                      .notes = draft.notes});
}
} // namespace fastecu::definition
