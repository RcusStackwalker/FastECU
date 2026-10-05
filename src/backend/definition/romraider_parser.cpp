#include "src/backend/definition/romraider_parser.h"
#include "src/backend/definition/parser_utils.h"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <pugixml.hpp>

#include "src/backend/ports/error.h"

using namespace std::literals::string_view_literals;

namespace fastecu::definition
{
namespace
{

void append_selections(pugi::xml_node parent, UnresolvedScaling& scaling)
{
    for (pugi::xml_node state : parent.children("state"))
    {
        scaling.selections.emplace_back(selection_name(value_or_empty(state.attribute("name"))),
                                        value_or_empty(state.attribute("data")));
    }
    for (pugi::xml_node data : parent.children("data"))
    {
        std::string value = value_or_empty(data.attribute("value"));
        if (value.empty())
        {
            value = value_or_empty(data.attribute("data"));
        }
        scaling.selections.emplace_back(selection_name(value_or_empty(data.attribute("name"))), std::move(value));
    }
}

Result<UnresolvedScaling> parse_scaling(pugi::xml_node scaling_node, std::string fallback_name, pugi::xml_node owner,
                                        std::string_view source, std::string_view definition_id)
{
    UnresolvedScaling scaling;
    scaling.name = value_or_empty(scaling_node.attribute("name"));
    if (scaling.name.empty())
    {
        scaling.name = std::move(fallback_name);
    }
    scaling.units = value_or_empty(scaling_node.attribute("units"));
    if (const auto expression = scaling_node.attribute("expression"))
    {
        scaling.from_byte = value_or_empty(expression);
    }
    if (const auto to_byte = scaling_node.attribute("to_byte"))
    {
        scaling.to_byte = value_or_empty(to_byte);
    }
    if (const auto format = scaling_node.attribute("format"))
    {
        scaling.format = value_or_empty(format);
    }
    scaling.minimum = value_or_empty(scaling_node.attribute("minimum"));
    if (scaling.minimum.empty())
    {
        scaling.minimum = value_or_empty(scaling_node.attribute("min"));
    }
    if (scaling.minimum.empty())
    {
        scaling.minimum = value_or_empty(owner.attribute("minvalue"));
    }
    scaling.maximum = value_or_empty(scaling_node.attribute("maximum"));
    if (scaling.maximum.empty())
    {
        scaling.maximum = value_or_empty(scaling_node.attribute("max"));
    }
    if (scaling.maximum.empty())
    {
        scaling.maximum = value_or_empty(owner.attribute("maxvalue"));
    }
    scaling.coarse_increment = value_or_empty(scaling_node.attribute("coarseincrement"));
    scaling.fine_increment = value_or_empty(scaling_node.attribute("fineincrement"));
    auto storage_type = optional_storage_type_attribute(scaling_node, "storagetype", source, definition_id);
    if (!storage_type.has_value())
    {
        return std::unexpected(storage_type.error());
    }
    scaling.storage_type = *storage_type;
    if (!scaling.storage_type)
    {
        auto owner_storage_type = optional_storage_type_attribute(owner, "storagetype", source, definition_id);
        if (!owner_storage_type.has_value())
        {
            return std::unexpected(owner_storage_type.error());
        }
        scaling.storage_type = *owner_storage_type;
    }
    scaling.endian = value_or_empty(scaling_node.attribute("endian"));
    if (scaling.endian.empty())
    {
        scaling.endian = value_or_empty(owner.attribute("endian"));
    }
    append_selections(scaling_node, scaling);
    return scaling;
}

Result<UnresolvedAxisDefinition> parse_axis(pugi::xml_node table, std::uint32_t default_size, std::string_view source,
                                            std::string_view definition_id, std::vector<UnresolvedScaling>& scalings)
{
    return parse_axis_definition(
        table, default_size, source, definition_id, scalings,
        [&](pugi::xml_node scaling_node, std::string fallback_name)
        { return parse_scaling(scaling_node, std::move(fallback_name), table, source, definition_id); });
}

Result<UnresolvedCalibrationMap> parse_table(pugi::xml_node table, std::string_view source,
                                             std::string_view definition_id, std::vector<UnresolvedScaling>& scalings)
{
    UnresolvedCalibrationMap map;
    if (auto status = populate_map_header(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = populate_map_size(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = populate_map_orientation(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }

    auto parsed_scaling = parse_map_scaling(
        table, map, [&](pugi::xml_node scaling_node, std::string fallback_name)
        { return parse_scaling(scaling_node, std::move(fallback_name), table, source, definition_id); });
    if (!parsed_scaling.has_value())
    {
        return std::unexpected(parsed_scaling.error());
    }
    std::optional<UnresolvedScaling>& scaling = *parsed_scaling;
    if (scaling.has_value())
    {
        append_selections(table, *scaling);
    }

    // A Switch is a selectable map whose <state> children are its selections.
    if (map.type == "Switch")
    {
        map.type = "Selectable";
        map.storage_type = StorageType::Bloblist;
        if (!scaling.has_value())
        {
            scaling.emplace();
            scaling->name = map_scaling_fallback_name(map);
            append_selections(table, *scaling);
            map.scaling_name = scaling->name;
        }
        scaling->storage_type = StorageType::Bloblist;
    }
    if (scaling.has_value())
    {
        scalings.push_back(std::move(*scaling));
    }

    if (auto status = populate_axes(table, map, source, definition_id, scalings, parse_axis); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    return map;
}

std::vector<std::string> parent_references(pugi::xml_node rom)
{
    const std::string parent = value_or_empty(rom.attribute("base"));
    return parent.empty() ? std::vector<std::string>{} : std::vector<std::string>{parent};
}

} // namespace

Result<std::vector<DefinitionIndexEntry>> parse_romraider_index(std::span<const std::uint8_t> xml,
                                                                std::string_view source)
{
    pugi::xml_document document;
    auto root = parse_root(document, xml, source, "roms"sv);
    if (!root.has_value())
    {
        return std::unexpected(root.error());
    }

    std::vector<DefinitionIndexEntry> entries;
    for (pugi::xml_node rom : root->children("rom"))
    {
        auto header = parse_rom_header(rom, source);
        if (!header)
        {
            return std::unexpected(header.error());
        }

        entries.push_back(DefinitionIndexEntry{
            .format = DefinitionFormat::RomRaider,
            .definition_id = std::move(header->identity.xml_id),
            .internal_id = std::move(header->identity.internal_id),
            .internal_id_address = header->identity.internal_id_address,
            .internal_id_encoding = IdEncoding::AsciiOrHex,
            .ecu_id = std::move(header->identity.ecu_id),
            .source = std::string{source},
            .parents = parent_references(rom),
        });
    }
    return entries;
}

Result<UnresolvedDefinition> parse_romraider_definition(std::span<const std::uint8_t> xml, std::string_view source,
                                                        std::string_view definition_id)
{
    if (definition_id.empty())
    {
        return invalid(source, "definition ID", "missing or empty required definition identity");
    }

    pugi::xml_document document;
    auto root = parse_root(document, xml, source, "roms"sv);
    if (!root.has_value())
    {
        return std::unexpected(root.error());
    }

    pugi::xml_node selected_rom;
    for (pugi::xml_node rom : root->children("rom"))
    {
        auto candidate_id = definition_id_for_rom(rom, source);
        if (!candidate_id)
        {
            return std::unexpected(candidate_id.error());
        }
        if (*candidate_id == definition_id)
        {
            if (selected_rom)
            {
                return invalid(source, "element <romid> child <xmlid>", "duplicate definition identity", definition_id);
            }
            selected_rom = rom;
        }
    }

    if (!selected_rom)
    {
        return invalid(source, "element <romid> child <xmlid>", "definition ID not found", definition_id);
    }

    auto header = parse_rom_header(selected_rom, source);
    if (!header)
    {
        return std::unexpected(header.error());
    }

    UnresolvedDefinition definition{
        .format = DefinitionFormat::RomRaider,
        .source = std::string{source},
        .identity = std::move(header->identity),
        .metadata = parse_metadata(header->rom_id),
        .parents = parent_references(selected_rom),
    };

    std::unordered_set<std::string> map_ids;
    for (pugi::xml_node table : selected_rom.children("table"))
    {
        auto map = parse_table(table, source, definition_id, definition.scalings);
        if (!map)
        {
            return std::unexpected(map.error());
        }
        if (const std::string map_id = map->id.value_or(map->name); !map_ids.insert(map_id).second)
        {
            return invalid(source, "element <table> attribute 'id' or 'name'",
                           std::format("duplicate map identity '{}'", map_id), definition_id);
        }
        definition.maps.push_back(std::move(*map));
    }
    return definition;
}

} // namespace fastecu::definition
