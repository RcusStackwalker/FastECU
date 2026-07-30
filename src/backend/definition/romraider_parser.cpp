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

Result<pugi::xml_node> identity_element(
    pugi::xml_node rom,
    std::string_view source)
{
    const pugi::xml_node rom_id = rom.child("romid");
    if (!rom_id)
    {
        return invalid(
            source,
            "element <rom> child <romid>",
            "missing required identity element");
    }
    if (rom_id.next_sibling("romid"))
    {
        return invalid(
            source,
            "element <rom> child <romid>",
            "duplicate singleton identity element");
    }

    static constexpr std::array singleton_children{
        "xmlid"sv,
        "internalidaddress"sv,
        "internalidstring"sv,
        "ecuid"sv,
        "make"sv,
        "market"sv,
        "model"sv,
        "submodel"sv,
        "transmission"sv,
        "year"sv,
        "flashmethod"sv,
        "memmodel"sv,
        "checksummodule"sv,
        "filesize"sv,
        "notes"sv,
    };
    for (const auto& child_name : singleton_children)
    {
        const pugi::xml_node child = rom_id.child(child_name);
        if (child && child.next_sibling(child_name))
        {
            return invalid(
                source,
                std::format("element <romid> child <{}>", child_name),
                "duplicate singleton identity element");
        }
    }
    return rom_id;
}

Result<std::optional<std::uint64_t>> optional_address(
    pugi::xml_node node,
    std::string_view source,
    std::string_view definition_id)
{
    if (node.attribute("address"))
    {
        return optional_hex_attribute(node, "address", source, definition_id);
    }
    return optional_hex_attribute(node, "storageaddress", source, definition_id);
}

void append_selections(pugi::xml_node parent, UnresolvedScaling& scaling)
{
    for (pugi::xml_node state : parent.children("state"))
    {
        scaling.selections.emplace_back(
            selection_name(value_or_empty(state.attribute("name"))),
            value_or_empty(state.attribute("data")));
    }
    for (pugi::xml_node data : parent.children("data"))
    {
        std::string value = value_or_empty(data.attribute("value"));
        if (value.empty())
        {
            value = value_or_empty(data.attribute("data"));
        }
        scaling.selections.emplace_back(
            selection_name(value_or_empty(data.attribute("name"))),
            std::move(value));
    }
}

UnresolvedScaling parse_scaling(
    pugi::xml_node scaling_node,
    std::string fallback_name,
    pugi::xml_node owner)
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
    scaling.storage_type = value_or_empty(scaling_node.attribute("storagetype"));
    if (scaling.storage_type.empty())
    {
        scaling.storage_type = value_or_empty(owner.attribute("storagetype"));
    }
    scaling.endian = value_or_empty(scaling_node.attribute("endian"));
    if (scaling.endian.empty())
    {
        scaling.endian = value_or_empty(owner.attribute("endian"));
    }
    append_selections(scaling_node, scaling);
    return scaling;
}

Result<UnresolvedAxisDefinition> parse_axis(
    pugi::xml_node table,
    std::uint32_t default_size,
    std::string_view source,
    std::string_view definition_id,
    std::vector<UnresolvedScaling>& scalings)
{
    UnresolvedAxisDefinition axis;
    axis.type = value_or_empty(table.attribute("type"));
    axis.name = value_or_empty(table.attribute("name"));
    if (axis.name.empty())
    {
        return invalid(
            source,
            "element <table> attribute 'name'",
            "missing or empty axis name",
            definition_id);
    }
    axis.storage_type = value_or_empty(table.attribute("storagetype"));
    axis.endian = value_or_empty(table.attribute("endian"));

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
    }
    else
    {
        axis.size = default_size;
    }

    axis.scaling_name = value_or_empty(table.attribute("scaling"));
    const pugi::xml_node scaling_node = table.child("scaling");
    if (scaling_node)
    {
        UnresolvedScaling scaling = parse_scaling(
            scaling_node,
            axis.scaling_name.empty() ? axis.name : axis.scaling_name,
            table);
        axis.scaling_name = scaling.name;
        axis.units = scaling.units;
        if (scaling.format)
        {
            axis.format = *scaling.format;
        }
        axis.from_byte = scaling.from_byte;
        axis.to_byte = scaling.to_byte;
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

Result<UnresolvedCalibrationMap> parse_table(
    pugi::xml_node table,
    std::string_view source,
    std::string_view definition_id,
    std::vector<UnresolvedScaling>& scalings)
{
    UnresolvedCalibrationMap map;
    if (const auto id = table.attribute("id"))
    {
        map.id = value_or_empty(id);
    }
    map.name = value_or_empty(table.attribute("name"));
    if (map.name.empty())
    {
        return invalid(
            source,
            "element <table> attribute 'name'",
            "missing or empty map name",
            definition_id);
    }
    map.type = value_or_empty(table.attribute("type"));
    map.category = value_or_empty(table.attribute("category"));
    map.subcategory = value_or_empty(table.attribute("subcategory"));
    map.description = value_or_empty(table.attribute("description"));
    if (map.description.empty())
    {
        map.description = child_text(table, "description");
    }
    map.level = value_or_empty(table.attribute("level"));
    map.user_level = value_or_empty(table.attribute("userlevel"));
    map.storage_type = value_or_empty(table.attribute("storagetype"));
    map.endian = value_or_empty(table.attribute("endian"));

    auto address = optional_address(table, source, definition_id);
    if (!address)
    {
        return std::unexpected(address.error());
    }
    map.address = *address;

    if (table.attribute("sizex"))
    {
        auto x_size = dimension_attribute(table, "sizex", 1, source, definition_id);
        if (!x_size)
        {
            return std::unexpected(x_size.error());
        }
        map.x_size = *x_size;
    }

    if (table.attribute("sizey"))
    {
        auto y_size = dimension_attribute(table, "sizey", 1, source, definition_id);
        if (!y_size)
        {
            return std::unexpected(y_size.error());
        }
        map.y_size = *y_size;
    }

    if (table.attribute("swapxy"))
    {
        auto swap_xy = strict_boolean_attribute(table, "swapxy", source, definition_id);
        if (!swap_xy)
        {
            return std::unexpected(swap_xy.error());
        }
        map.swap_xy = *swap_xy;
    }

    if (table.attribute("flipx"))
    {
        auto flip_x = strict_boolean_attribute(table, "flipx", source, definition_id);
        if (!flip_x)
        {
            return std::unexpected(flip_x.error());
        }
        map.flip_x = *flip_x;
    }

    if (table.attribute("flipy"))
    {
        auto flip_y = strict_boolean_attribute(table, "flipy", source, definition_id);
        if (!flip_y)
        {
            return std::unexpected(flip_y.error());
        }
        map.flip_y = *flip_y;
    }

    map.scaling_name = value_or_empty(table.attribute("scaling"));
    const pugi::xml_node scaling_node = table.child("scaling");
    if (scaling_node)
    {
        UnresolvedScaling scaling = parse_scaling(
            scaling_node,
            map.scaling_name.empty() ? map.id.value_or(map.name) : map.scaling_name,
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
            UnresolvedScaling scaling;
            scaling.name =
                map.scaling_name.empty() ? map.id.value_or(map.name) : map.scaling_name;
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
        const std::string type = value_or_empty(axis_table.attribute("type"));
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
                map.x_size = map.y_size;
                map.y_size = 1;
            }
            auto axis = parse_axis(
                axis_table, map.x_size.value_or(1U), source, definition_id, scalings);
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
                axis_table, map.y_size.value_or(1U), source, definition_id, scalings);
            if (!axis)
            {
                return std::unexpected(axis.error());
            }
            map.y_axis = std::move(*axis);
        }
    }
    return map;
}

Result<std::string> definition_id_for_rom(pugi::xml_node rom, std::string_view source)
{
    auto rom_id = identity_element(rom, source);
    if (!rom_id)
    {
        return std::unexpected(rom_id.error());
    }
    return required_child_text(*rom_id, "romid", "xmlid", source);
}

std::vector<std::string> parent_references(pugi::xml_node rom)
{
    const std::string parent = value_or_empty(rom.attribute("base"));
    return parent.empty() ? std::vector<std::string>{}
                          : std::vector<std::string>{parent};
}

RomMetadata parse_metadata(pugi::xml_node rom_id)
{
    return RomMetadata{
        .make = child_text(rom_id, "make"),
        .market = child_text(rom_id, "market"),
        .model = child_text(rom_id, "model"),
        .submodel = child_text(rom_id, "submodel"),
        .transmission = child_text(rom_id, "transmission"),
        .year = child_text(rom_id, "year"),
        .flash_method = child_text(rom_id, "flashmethod"),
        .memory_model = child_text(rom_id, "memmodel"),
        .checksum_module = child_text(rom_id, "checksummodule"),
        .file_size = child_text(rom_id, "filesize"),
        .notes = child_text(rom_id, "notes"),
    };
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
        auto internal_id_address =
            optional_hex_element(rom_id, "internalidaddress", source, *definition_id);
        if (!internal_id_address)
        {
            return std::unexpected(internal_id_address.error());
        }

        entries.push_back(DefinitionIndexEntry{
            .format = DefinitionFormat::RomRaider,
            .definition_id = std::move(*definition_id),
            .internal_id = child_text(rom_id, "internalidstring"),
            .internal_id_address = *internal_id_address,
            .internal_id_encoding = IdEncoding::Ascii,
            .ecu_id = child_text(rom_id, "ecuid"),
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
    auto internal_id_address =
        optional_hex_element(rom_id, "internalidaddress", source, definition_id);
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
            return invalid(
                source,
                "element <table> attribute 'id' or 'name'",
                "duplicate map identity '" + map_id + "'",
                definition_id);
        }
        definition.maps.push_back(std::move(*map));
    }
    return definition;
}

} // namespace fastecu::definition
