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
    if (!scaling.storage_type.has_value())
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
    UnresolvedAxisDefinition axis;
    if (auto status = populate_common_axis_attributes(table, axis, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (axis.name.empty())
    {
        return invalid(source, "element <table> attribute 'name'", "missing or empty axis name", definition_id);
    }
    auto address = optional_address(table, source, definition_id);
    if (!address)
    {
        return std::unexpected(address.error());
    }
    axis.address = *address;

    const char *size_attribute =
        table.attribute("elements") ? "elements" : (table.attribute("size") ? "size" : nullptr);
    if (size_attribute)
    {
        auto size = dimension_attribute(table, size_attribute, default_size, source, definition_id);
        if (!size)
        {
            return std::unexpected(size.error());
        }
        axis.size = *size;
    }
    axis.scaling_name = value_or_empty(table.attribute("scaling"));
    const pugi::xml_node scaling_node = table.child("scaling");
    if (scaling_node)
    {
        auto parsed_scaling = parse_scaling(scaling_node, axis.scaling_name.empty() ? axis.name : axis.scaling_name,
                                            table, source, definition_id);
        if (!parsed_scaling.has_value())
        {
            return std::unexpected(parsed_scaling.error());
        }
        apply_scaling_to_axis(*parsed_scaling, axis);
        scalings.push_back(std::move(*parsed_scaling));
    }
    return axis;
}

Result<UnresolvedCalibrationMap> parse_table(pugi::xml_node table, std::string_view source,
                                             std::string_view definition_id, std::vector<UnresolvedScaling>& scalings)
{
    UnresolvedCalibrationMap map;
    if (auto status = populate_common_map_attributes(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (map.name.empty())
    {
        return invalid(source, "element <table> attribute 'name'", "missing or empty map name", definition_id);
    }
    auto address = optional_address(table, source, definition_id);
    if (!address)
    {
        return std::unexpected(address.error());
    }
    map.address = *address;

    if (auto status = populate_optional_dimension(table, "sizex", map.x_size, source, definition_id); !status)
    {
        return std::unexpected(status.error());
    }

    if (auto status = populate_optional_dimension(table, "sizey", map.y_size, source, definition_id); !status)
    {
        return std::unexpected(status.error());
    }

    if (auto status = populate_optional_boolean(table, "swapxy", map.swap_xy, source, definition_id); !status)
    {
        return std::unexpected(status.error());
    }

    if (auto status = populate_optional_boolean(table, "flipx", map.flip_x, source, definition_id); !status)
    {
        return std::unexpected(status.error());
    }

    if (auto status = populate_optional_boolean(table, "flipy", map.flip_y, source, definition_id); !status)
    {
        return std::unexpected(status.error());
    }

    map.scaling_name = value_or_empty(table.attribute("scaling"));
    const pugi::xml_node scaling_node = table.child("scaling");
    if (scaling_node)
    {
        auto parsed_scaling =
            parse_scaling(scaling_node, map.scaling_name.empty() ? map.id.value_or(map.name) : map.scaling_name, table,
                          source, definition_id);
        if (!parsed_scaling.has_value())
        {
            return std::unexpected(parsed_scaling.error());
        }
        UnresolvedScaling& scaling = *parsed_scaling;
        map.scaling_name = scaling.name;
        if (!map.storage_type.has_value())
        {
            map.storage_type = scaling.storage_type;
        }
        if (map.endian.empty())
        {
            map.endian = scaling.endian;
        }
        append_selections(table, scaling);
        scalings.push_back(std::move(scaling));
    }

    if (map.type == "Switch")
    {
        map.type = "Selectable";
        map.storage_type = StorageType::Bloblist;
        if (!scaling_node)
        {
            UnresolvedScaling scaling;
            scaling.name = map.scaling_name.empty() ? map.id.value_or(map.name) : map.scaling_name;
            scaling.storage_type = StorageType::Bloblist;
            append_selections(table, scaling);
            map.scaling_name = scaling.name;
            scalings.push_back(std::move(scaling));
        }
        else
        {
            scalings.back().storage_type = StorageType::Bloblist;
        }
    }

    if (auto status = populate_axes(table, map, source, definition_id, scalings, parse_axis); !status)
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
        auto definition_id = definition_id_for_rom(rom, source);
        if (!definition_id)
        {
            return std::unexpected(definition_id.error());
        }

        const pugi::xml_node rom_id = rom.child("romid");
        auto internal_id_address = optional_hex_element(rom_id, "internalidaddress", source, *definition_id);
        if (!internal_id_address)
        {
            return std::unexpected(internal_id_address.error());
        }

        entries.push_back(DefinitionIndexEntry{
            .format = DefinitionFormat::RomRaider,
            .definition_id = std::move(*definition_id),
            .internal_id = child_text(rom_id, "internalidstring"),
            .internal_id_address = *internal_id_address,
            .internal_id_encoding = IdEncoding::AsciiOrHex,
            .ecu_id = child_text(rom_id, "ecuid"),
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

    const pugi::xml_node rom_id = selected_rom.child("romid");
    auto internal_id_address = optional_hex_element(rom_id, "internalidaddress", source, definition_id);
    if (!internal_id_address)
    {
        return std::unexpected(internal_id_address.error());
    }

    UnresolvedDefinition definition{
        .format = DefinitionFormat::RomRaider,
        .source = std::string{source},
        .identity =
            RomIdentity{
                .xml_id = std::string{definition_id},
                .internal_id = child_text(rom_id, "internalidstring"),
                .ecu_id = child_text(rom_id, "ecuid"),
                .internal_id_address = *internal_id_address,
            },
        .metadata = parse_metadata(rom_id),
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
        const std::string map_id = map->id.value_or(map->name);
        if (!map_ids.insert(map_id).second)
        {
            return invalid(source, "element <table> attribute 'id' or 'name'",
                           std::format("duplicate map identity '{}'", map_id), definition_id);
        }
        definition.maps.push_back(std::move(*map));
    }
    return definition;
}

} // namespace fastecu::definition
