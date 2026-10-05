#include "src/backend/definition/definition_header_fields.h"

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>

#include <pugixml.hpp>

#include "src/backend/definition/text_format.h"
#include "src/backend/definition/parser_utils.h"
#include "src/backend/definition/metadata_fields.h"

namespace fastecu::definition
{
DefinitionHeaderFields collect_ecuflash_base_header_fields(std::span<const std::string> names, std::string_view xml)
{
    pugi::xml_document document;
    pugi::xml_node root;
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t *>(xml.data()), xml.size()};
    const auto parsed = parse_document_root(document, bytes, "authoring header", pugi::encoding_utf8);
    if (parsed.has_value() &&
        std::ranges::count_if(document.children(), [](auto node) { return node.type() == pugi::node_element; }) == 1)
    {
        const auto document_root = *parsed;
        const std::string_view name{document_root.name()};
        if (name == "rom")
        {
            root = document_root;
        }
        else if (name == "roms")
        {
            root = document_root.child("rom");
        }
    }
    const auto rom_id = root.child("romid");
    DefinitionHeaderFields fields;
    fields.reserve(names.size());
    for (const auto& name : names)
    {
        const auto element =
            (name == "include" || name == "notes") ? root.child(name.c_str()) : rom_id.child(name.c_str());
        fields.emplace_back(name, header_child_text(element.parent(), name));
    }
    return fields;
}

Result<DefinitionHeaderInput> definition_header_input(std::span<const std::pair<std::string, std::string>> fields)
{
    const auto value = [fields](std::string_view name) -> std::string_view
    {
        for (const auto& [field_name, text] : fields | std::views::reverse)
        {
            if (field_name == name)
            {
                return text;
            }
        }
        return {};
    };
    const auto address = parse_header_address(value("internalidaddress"), "authoring header");
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    RomMetadata metadata;
    for (const auto& field : kEditableMetadataFields)
    {
        metadata.*field.member = trim_header_text(value(field.xml_name));
    }
    return DefinitionHeaderInput{.xml_id = std::string{trim_header_text(value("xmlid"))},
                                 .internal_id = std::string{trim_header_text(value("internalidstring"))},
                                 .ecu_id = std::string{trim_header_text(value("ecuid"))},
                                 .internal_id_address = *address,
                                 .metadata = std::move(metadata),
                                 .include = std::string{trim_header_text(value("include"))},
                                 .notes = std::string{value("notes")}};
}
} // namespace fastecu::definition
