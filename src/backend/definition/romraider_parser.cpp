#include "src/backend/definition/romraider_parser.h"
#include "src/backend/definition/parser_utils.h"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
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

void append_selections(pugi::xml_node parent, Scaling& scaling)
{
    for (pugi::xml_node state : parent.children("state"))
    {
        scaling.selections.emplace_back(
            selection_name(attribute_or_empty(state, "name")),
            attribute_or_empty(state, "data"));
    }
    append_data_selections(parent, scaling);
}

Scaling parse_scaling(
    pugi::xml_node scaling_node,
    std::string fallback_name,
    pugi::xml_node owner)
{
    Scaling scaling;
    scaling.supplied.tracked = true;
    scaling.name = attribute_or_empty(scaling_node, "name");
    if (scaling.name.empty())
    {
        scaling.name = std::move(fallback_name);
    }
    scaling.units = attribute_or_empty(scaling_node, "units");
    scaling.from_byte = attribute_or_empty(scaling_node, "expression");
    scaling.supplied.from_byte = static_cast<bool>(scaling_node.attribute("expression"));
    if (scaling.from_byte.empty())
    {
        scaling.from_byte = "x";
    }
    scaling.to_byte = attribute_or_empty(scaling_node, "to_byte");
    scaling.supplied.to_byte = static_cast<bool>(scaling_node.attribute("to_byte"));
    if (scaling.to_byte.empty())
    {
        scaling.to_byte = "x";
    }
    scaling.format = attribute_or_empty(scaling_node, "format");
    scaling.supplied.format = static_cast<bool>(scaling_node.attribute("format"));
    scaling.minimum = attribute_or_empty(scaling_node, "minimum");
    if (scaling.minimum.empty())
    {
        scaling.minimum = attribute_or_empty(scaling_node, "min");
    }
    if (scaling.minimum.empty())
    {
        scaling.minimum = attribute_or_empty(owner, "minvalue");
    }
    scaling.maximum = attribute_or_empty(scaling_node, "maximum");
    if (scaling.maximum.empty())
    {
        scaling.maximum = attribute_or_empty(scaling_node, "max");
    }
    if (scaling.maximum.empty())
    {
        scaling.maximum = attribute_or_empty(owner, "maxvalue");
    }
    scaling.coarse_increment = attribute_or_empty(scaling_node, "coarseincrement");
    scaling.fine_increment = attribute_or_empty(scaling_node, "fineincrement");
    scaling.storage_type = attribute_or_empty(scaling_node, "storagetype");
    if (scaling.storage_type.empty())
    {
        scaling.storage_type = attribute_or_empty(owner, "storagetype");
    }
    scaling.endian = attribute_or_empty(scaling_node, "endian");
    if (scaling.endian.empty())
    {
        scaling.endian = attribute_or_empty(owner, "endian");
    }
    append_selections(scaling_node, scaling);
    return scaling;
}

Result<AxisDefinition> parse_axis(
    pugi::xml_node table,
    std::uint32_t default_size,
    std::string_view source,
    std::string_view definition_id,
    std::vector<Scaling>& scalings)
{
    AxisDefinition axis;
    populate_common_axis_attributes(table, axis);
    if (axis.name.empty())
    {
        return invalid(
            source,
            "element <table> attribute 'name'",
            "missing or empty axis name",
            definition_id);
    }
    if (table.attribute("logparam"))
    {
        axis.log_parameter = attribute_or_empty(table, "logparam");
        axis.supplied.log_parameter = true;
    }
    auto address = optional_address(table, source, definition_id);
    if (!address)
    {
        return std::unexpected(address.error());
    }
    axis.address = *address;

    const char *size_attribute = table.attribute("elements")
                                     ? "elements"
                                     : (table.attribute("size") ? "size" : nullptr);
    if (size_attribute)
    {
        auto size = dimension_attribute(table, size_attribute, default_size, source, definition_id);
        if (!size)
        {
            return std::unexpected(size.error());
        }
        axis.size = *size;
        axis.supplied.size = true;
    }
    else
    {
        axis.size = default_size;
    }

    axis.scaling_name = attribute_or_empty(table, "scaling");
    if (const pugi::xml_node scaling_node = table.child("scaling"))
    {
        Scaling scaling = parse_scaling(
            scaling_node,
            axis.scaling_name.empty() ? axis.name : axis.scaling_name,
            table);
        axis.scaling_name = scaling.name;
        axis.units = scaling.units;
        axis.format = scaling.format;
        axis.from_byte = scaling.from_byte;
        axis.to_byte = scaling.to_byte;
        axis.supplied.from_byte = static_cast<bool>(scaling_node.attribute("expression"));
        axis.supplied.to_byte = static_cast<bool>(scaling_node.attribute("to_byte"));
        if (axis.storage_type.empty())
        {
            axis.storage_type = scaling.storage_type;
        }
        if (axis.endian.empty())
        {
            axis.endian = scaling.endian;
        }
        scalings.push_back(std::move(scaling));
    }
    return axis;
}

Result<CalibrationMap> parse_table(
    pugi::xml_node table,
    std::string_view source,
    std::string_view definition_id,
    std::vector<Scaling>& scalings)
{
    CalibrationMap map;
    populate_common_map_attributes(table, map);
    if (map.name.empty())
    {
        return invalid(
            source,
            "element <table> attribute 'name'",
            "missing or empty map name",
            definition_id);
    }
    if (table.attribute("logparam"))
    {
        map.log_parameter = attribute_or_empty(table, "logparam");
        map.supplied.log_parameter = true;
    }

    auto address = optional_address(table, source, definition_id);
    if (!address)
    {
        return std::unexpected(address.error());
    }
    map.address = *address;

    auto x_size = dimension_attribute(table, "sizex", 1, source, definition_id);
    if (!x_size.has_value())
    {
        return std::unexpected(x_size.error());
    }
    map.x_size = *x_size;
    map.supplied.x_size = static_cast<bool>(table.attribute("sizex"));

    auto y_size = dimension_attribute(table, "sizey", 1, source, definition_id);
    if (!y_size.has_value())
    {
        return std::unexpected(y_size.error());
    }
    map.y_size = *y_size;
    map.supplied.y_size = static_cast<bool>(table.attribute("sizey"));

    auto swap_xy = strict_boolean_attribute(table, "swapxy", source, definition_id);
    if (!swap_xy.has_value())
    {
        return std::unexpected(swap_xy.error());
    }
    map.swap_xy = *swap_xy;
    map.supplied.swap_xy = static_cast<bool>(table.attribute("swapxy"));

    auto flip_x = strict_boolean_attribute(table, "flipx", source, definition_id);
    if (!flip_x.has_value())
    {
        return std::unexpected(flip_x.error());
    }
    map.flip_x = *flip_x;
    map.supplied.flip_x = static_cast<bool>(table.attribute("flipx"));

    auto flip_y = strict_boolean_attribute(table, "flipy", source, definition_id);
    if (!flip_y.has_value())
    {
        return std::unexpected(flip_y.error());
    }
    map.flip_y = *flip_y;
    map.supplied.flip_y = static_cast<bool>(table.attribute("flipy"));

    map.scaling_name = attribute_or_empty(table, "scaling");
    const pugi::xml_node scaling_node = table.child("scaling");
    if (scaling_node)
    {
        Scaling scaling = parse_scaling(
            scaling_node,
            map.scaling_name.empty() ? map.id : map.scaling_name,
            table);
        map.scaling_name = scaling.name;
        if (map.storage_type.empty())
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
        map.storage_type = "bloblist";
        if (!scaling_node)
        {
            Scaling scaling;
            scaling.name = map.scaling_name.empty() ? map.id : map.scaling_name;
            scaling.storage_type = "bloblist";
            append_selections(table, scaling);
            map.scaling_name = scaling.name;
            scalings.push_back(std::move(scaling));
        }
        else
        {
            scalings.back().storage_type = "bloblist";
        }
    }

    bool x_axis_populated = false;
    bool y_axis_populated = false;
    for (pugi::xml_node axis_table : table.children("table"))
    {
        const std::string type = attribute_or_empty(axis_table, "type");
        if (type == "X Axis" || type == "Static X Axis" || type == "Static Y Axis" || (type == "Y Axis" && map.type == "2D"))
        {
            if (x_axis_populated)
            {
                return invalid(
                    source,
                    "element <table> child <table> type '" + type + "'",
                    "second axis targets already-populated X axis slot",
                    definition_id);
            }
            x_axis_populated = true;
            if (type == "Static Y Axis" || type == "Y Axis")
            {
                const bool dimension_supplied = map.supplied.y_size;
                map.x_size = map.y_size;
                map.y_size = 1;
                map.supplied.x_size = dimension_supplied;
                map.supplied.y_size = dimension_supplied;
            }
            auto axis = parse_axis(
                axis_table, map.x_size, source, definition_id, scalings);
            if (!axis)
            {
                return std::unexpected(axis.error());
            }
            if (axis->type == "Static Y Axis")
            {
                axis->type = "Static X Axis";
            }
            map.x_axis = std::move(*axis);
        }
        else if (type == "Y Axis")
        {
            if (y_axis_populated)
            {
                return invalid(
                    source,
                    "element <table> child <table> type '" + type + "'",
                    "second axis targets already-populated Y axis slot",
                    definition_id);
            }
            y_axis_populated = true;
            auto axis = parse_axis(
                axis_table, map.y_size, source, definition_id, scalings);
            if (!axis)
            {
                return std::unexpected(axis.error());
            }
            map.y_axis = std::move(*axis);
        }
    }
    return map;
}

std::vector<std::string> parent_references(pugi::xml_node rom)
{
    const std::string parent = attribute_or_empty(rom, "base");
    return parent.empty() ? std::vector<std::string>{}
                          : std::vector<std::string>{parent};
}

} // namespace

Result<std::vector<DefinitionIndexEntry>> parse_romraider_index(
    std::span<const std::uint8_t> xml, std::string_view source)
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
        auto identity = parse_identity(
            rom_id, source, std::move(*definition_id));
        if (!identity)
        {
            return std::unexpected(identity.error());
        }

        entries.push_back(DefinitionIndexEntry{
            .format = DefinitionFormat::RomRaider,
            .definition_id = std::move(identity->xml_id),
            .internal_id = std::move(identity->internal_id),
            .internal_id_address = identity->internal_id_address,
            .internal_id_encoding = IdEncoding::AsciiOrHex,
            .ecu_id = std::move(identity->ecu_id),
            .source = std::string{source},
            .parents = parent_references(rom),
        });
    }
    return entries;
}

Result<UnresolvedDefinition> parse_romraider_definition(
    std::span<const std::uint8_t> xml,
    std::string_view source,
    std::string_view definition_id)
{
    if (definition_id.empty())
    {
        return invalid(
            source,
            "definition ID",
            "missing or empty required definition identity");
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
                return invalid(
                    source,
                    "element <romid> child <xmlid>",
                    "duplicate definition identity",
                    definition_id);
            }
            selected_rom = rom;
        }
    }

    if (!selected_rom)
    {
        return invalid(
            source,
            "element <romid> child <xmlid>",
            "definition ID not found",
            definition_id);
    }

    const pugi::xml_node rom_id = selected_rom.child("romid");
    auto identity = parse_identity(rom_id, source, std::string{definition_id});
    if (!identity)
    {
        return std::unexpected(identity.error());
    }

    UnresolvedDefinition definition{
        .format = DefinitionFormat::RomRaider,
        .source = std::string{source},
        .identity = std::move(*identity),
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
        auto appended = append_unique_map(
            definition, std::move(*map), map_ids, source);
        if (!appended)
        {
            return std::unexpected(appended.error());
        }
    }
    return definition;
}

} // namespace fastecu::definition
