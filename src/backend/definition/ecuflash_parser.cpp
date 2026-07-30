#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/definition/parser_utils.h"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
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
        return invalid(source, "element <rom> child <romid>", "missing required identity element");
    }
    if (rom_id.next_sibling("romid"))
    {
        return invalid(
            source,
            "element <rom> child <romid>",
            "duplicate singleton identity element");
    }

    static constexpr std::array singleton_children{
        "xmlid",
        "internalidaddress",
        "internalidstring",
        "ecuid",
        "make",
        "market",
        "model",
        "submodel",
        "transmission",
        "year",
        "flashmethod",
        "memmodel",
        "checksummodule",
        "filesize",
        "notes",
    };
    for (const char *child_name : singleton_children)
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

Result<std::string> definition_id_for_rom(pugi::xml_node rom, std::string_view source)
{
    auto rom_id = identity_element(rom, source);
    if (!rom_id)
    {
        return std::unexpected(rom_id.error());
    }
    return required_child_text(*rom_id, "romid", "xmlid", source);
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

std::string convert_value_format(std::string_view format)
{
    const std::size_t dot = format.find('.');
    const std::size_t f = format.find('f', dot == std::string_view::npos ? 0 : dot + 1);
    if (dot == std::string_view::npos || f == std::string_view::npos || f <= dot + 1)
    {
        return "0";
    }

    std::uint64_t precision = 0;
    for (std::size_t index = dot + 1; index < f; ++index)
    {
        const char character = format[index];
        if (!std::isdigit(static_cast<unsigned char>(character)))
        {
            return "0";
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (precision > std::numeric_limits<std::uint64_t>::max() / 10 ||
            (precision == std::numeric_limits<std::uint64_t>::max() / 10 &&
             digit > std::numeric_limits<std::uint64_t>::max() % 10))
        {
            return "0";
        }
        precision = precision * 10 + digit;
    }
    if (precision == 0 || precision > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    {
        return "0";
    }
    return "0." + std::string(static_cast<std::size_t>(precision), '0');
}

std::string fine_increment_from(std::string_view coarse_increment)
{
    const std::string value = trim_copy(coarse_increment);
    if (value.empty())
    {
        return {};
    }

    char *end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0')
    {
        return "0";
    }
    std::ostringstream result;
    result << std::setprecision(15) << parsed / 10.0;
    return result.str();
}

void append_selections(pugi::xml_node parent, UnresolvedScaling& scaling)
{
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

UnresolvedScaling parse_scaling(pugi::xml_node node, std::string fallback_name)
{
    UnresolvedScaling scaling;
    scaling.name = value_or_empty(node.attribute("name"));
    if (scaling.name.empty())
    {
        scaling.name = std::move(fallback_name);
    }
    scaling.units = value_or_empty(node.attribute("units"));
    if (const auto toexpr = node.attribute("toexpr"))
    {
        scaling.from_byte = value_or_empty(toexpr);
    }
    if (const auto frexpr = node.attribute("frexpr"))
    {
        scaling.to_byte = value_or_empty(frexpr);
    }
    if (const auto format = node.attribute("format"))
    {
        scaling.format = convert_value_format(value_or_empty(format));
    }
    scaling.minimum = value_or_empty(node.attribute("min"));
    scaling.maximum = value_or_empty(node.attribute("max"));
    scaling.coarse_increment = value_or_empty(node.attribute("inc"));
    scaling.fine_increment = fine_increment_from(scaling.coarse_increment);
    scaling.storage_type = value_or_empty(node.attribute("storagetype"));
    scaling.endian = value_or_empty(node.attribute("endian"));
    append_selections(node, scaling);
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
            "element <table> type '" + axis.type + "' attribute 'name'",
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

    const char *size_attribute = table.attribute("elements") ? "elements" : (table.attribute("size") ? "size" : nullptr);
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
    if (const pugi::xml_node scaling_node = table.child("scaling"))
    {
        UnresolvedScaling scaling = parse_scaling(
            scaling_node, axis.scaling_name.empty() ? axis.name : axis.scaling_name);
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
        return invalid(source, "element <table> attribute 'name'", "missing or empty map name", definition_id);
    }
    map.type = value_or_empty(table.attribute("type"));
    const bool is_top_level_x_axis = map.type == "X Axis";
    const bool is_top_level_y_axis = map.type == "Y Axis";
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

    if (is_top_level_x_axis || is_top_level_y_axis)
    {
        auto elements = dimension_attribute(table, "elements", 1, source, definition_id);
        if (!elements)
        {
            return std::unexpected(elements.error());
        }
        map.type = "2D";
        map.x_size = is_top_level_x_axis ? *elements : 1;
        map.y_size = is_top_level_y_axis ? *elements : 1;
    }
    else
    {
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
    if (const pugi::xml_node scaling_node = table.child("scaling"))
    {
        UnresolvedScaling scaling = parse_scaling(
            scaling_node,
            map.scaling_name.empty() ? map.id.value_or(map.name) : map.scaling_name);
        map.scaling_name = scaling.name;
        if (map.storage_type.empty())
        {
            map.storage_type = scaling.storage_type;
        }
        if (map.endian.empty())
        {
            map.endian = scaling.endian;
        }
        if (scaling.storage_type == "bloblist")
        {
            map.type = "Selectable";
        }
        scalings.push_back(std::move(scaling));
    }

    bool x_axis_populated = false;
    bool y_axis_populated = false;
    for (pugi::xml_node axis_table : table.children("table"))
    {
        const std::string type = value_or_empty(axis_table.attribute("type"));
        if (type == "X Axis" || type == "Static X Axis" || type == "Static Y Axis" ||
            (type == "Y Axis" && map.type == "2D"))
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

std::vector<std::string> parent_references(pugi::xml_node rom)
{
    std::vector<std::string> parents;
    for (pugi::xml_node include : rom.children("include"))
    {
        const std::string parent = trim_copy(include.child_value());
        if (!parent.empty())
        {
            parents.push_back(parent);
        }
    }
    return parents;
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

Result<std::vector<DefinitionIndexEntry>> parse_ecuflash_index(
    std::span<const std::uint8_t> xml, std::string_view source)
{
    pugi::xml_document document;
    auto root = parse_root(document, xml, source, "rom"sv);
    if (!root.has_value())
    {
        return std::unexpected(root.error());
    }
    auto definition_id = definition_id_for_rom(*root, source);
    if (!definition_id)
    {
        return std::unexpected(definition_id.error());
    }
    const pugi::xml_node rom_id = root->child("romid");
    auto internal_id_address = optional_hex_element(rom_id, "internalidaddress", source, *definition_id);
    if (!internal_id_address)
    {
        return std::unexpected(internal_id_address.error());
    }

    return std::vector<DefinitionIndexEntry>{DefinitionIndexEntry{
        .format = DefinitionFormat::EcuFlash,
        .definition_id = std::move(*definition_id),
        .internal_id = child_text(rom_id, "internalidstring"),
        .internal_id_address = *internal_id_address,
        .internal_id_encoding = IdEncoding::Ascii,
        .ecu_id = child_text(rom_id, "ecuid"),
        .source = std::string(source),
        .parents = parent_references(*root),
    }};
}

Result<UnresolvedDefinition> parse_ecuflash_definition(
    std::span<const std::uint8_t> xml, std::string_view source)
{
    pugi::xml_document document;
    auto root = parse_root(document, xml, source, "rom"sv);
    if (!root.has_value())
    {
        return std::unexpected(root.error());
    }
    auto definition_id = definition_id_for_rom(*root, source);
    if (!definition_id)
    {
        return std::unexpected(definition_id.error());
    }
    const pugi::xml_node rom_id = root->child("romid");
    auto internal_id_address = optional_hex_element(rom_id, "internalidaddress", source, *definition_id);
    if (!internal_id_address)
    {
        return std::unexpected(internal_id_address.error());
    }

    UnresolvedDefinition definition{
        .format = DefinitionFormat::EcuFlash,
        .source = std::string(source),
        .identity = RomIdentity{
            .xml_id = std::move(*definition_id),
            .internal_id = child_text(rom_id, "internalidstring"),
            .ecu_id = child_text(rom_id, "ecuid"),
            .internal_id_address = *internal_id_address,
        },
        .metadata = parse_metadata(rom_id),
        .parents = parent_references(*root),
    };

    std::unordered_map<std::string, UnresolvedScaling> global_scalings;
    for (pugi::xml_node scaling_node : root->children("scaling"))
    {
        UnresolvedScaling scaling = parse_scaling(scaling_node, {});
        if (!scaling.name.empty())
        {
            const auto [existing, inserted] = global_scalings.emplace(scaling.name, scaling);
            if (!inserted && existing->second != scaling)
            {
                return invalid(
                    source,
                    "element <scaling> attribute 'name'",
                    "conflicting duplicate global scaling '" + scaling.name + "'",
                    definition.identity.xml_id);
            }
            if (!inserted)
            {
                continue;
            }
        }
        definition.scalings.push_back(std::move(scaling));
    }

    std::unordered_set<std::string> map_ids;
    for (pugi::xml_node table : root->children("table"))
    {
        auto map = parse_table(table, source, definition.identity.xml_id, definition.scalings);
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
                definition.identity.xml_id);
        }
        definition.maps.push_back(std::move(*map));
    }
    return definition;
}

} // namespace fastecu::definition
