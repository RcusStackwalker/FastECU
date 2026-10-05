#include "src/backend/definition/definition_header_fields.h"

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>

#include <pugixml.hpp>

#include "src/backend/definition/parser_utils.h"
#include "src/backend/definition/metadata_fields.h"

namespace fastecu::definition
{
Result<DefinitionHeaderFields> collect_ecuflash_base_header_fields(std::span<const std::string> names,
                                                                   std::string_view xml)
{
    pugi::xml_document document;
    pugi::xml_node root;
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t *>(xml.data()), xml.size()};
    const auto parsed = parse_document_root(document, bytes, "authoring header", pugi::encoding_utf8);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    if (std::ranges::count_if(document.children(), [](auto node) { return node.type() == pugi::node_element; }) != 1)
    {
        return invalid("authoring header", "XML document", "expected one document root");
    }
    const std::string_view root_name{parsed->name()};
    if (root_name == "rom")
    {
        root = *parsed;
    }
    else if (root_name == "roms")
    {
        root = parsed->child("rom");
    }
    if (!root)
    {
        return invalid("authoring header", "XML document", "expected <rom> or a direct <rom> child of <roms>");
    }
    if (auto status = validate_header_structure(root, "authoring header"); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    const auto rom_id = root.child("romid");
    DefinitionHeaderFields fields;
    fields.reserve(names.size());
    for (const auto& name : names)
    {
        const bool metadata =
            std::ranges::any_of(kEditableMetadataFields, [&name](auto field) { return field.xml_name == name; });
        if (!metadata && name != "xmlid" && name != "internalidstring" && name != "internalidaddress" &&
            name != "ecuid" && name != "include" && name != "notes")
        {
            return invalid("authoring header", "field name", std::format("unsupported field '{}'", name));
        }
        fields.emplace_back(name, header_child_text(name == "include" || name == "notes" ? root : rom_id, name));
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
        metadata.*field.member = value(field.xml_name);
    }
    return normalize_header_input(DefinitionHeaderInput{.xml_id = std::string{value("xmlid")},
                                                        .internal_id = std::string{value("internalidstring")},
                                                        .ecu_id = std::string{value("ecuid")},
                                                        .internal_id_address = *address,
                                                        .metadata = std::move(metadata),
                                                        .include = std::string{value("include")},
                                                        .notes = std::string{value("notes")}});
}
} // namespace fastecu::definition
