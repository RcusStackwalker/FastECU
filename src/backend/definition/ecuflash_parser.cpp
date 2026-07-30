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
        const auto digit = static_cast<std::uint64_t>(character - '0');
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

    double parsed;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size())
    {
        return "0";
    }
    std::ostringstream result;
    result << std::setprecision(15) << parsed / 10.0;
    return result.str();
}

void append_selections(pugi::xml_node parent, Scaling& scaling)
{
    append_data_selections(parent, scaling);
}

Scaling parse_scaling(pugi::xml_node node, std::string fallback_name)
{
    Scaling scaling;
    scaling.supplied.tracked = true;
    scaling.name = attribute_or_empty(node, "name");
    if (scaling.name.empty())
    {
        scaling.name = std::move(fallback_name);
    }
    scaling.units = attribute_or_empty(node, "units");
    scaling.from_byte = attribute_or_empty(node, "toexpr");
    scaling.supplied.from_byte = static_cast<bool>(node.attribute("toexpr"));
    if (scaling.from_byte.empty())
    {
        scaling.from_byte = "x";
    }
    scaling.to_byte = attribute_or_empty(node, "frexpr");
    scaling.supplied.to_byte = static_cast<bool>(node.attribute("frexpr"));
    if (scaling.to_byte.empty())
    {
        scaling.to_byte = "x";
    }
    scaling.format = convert_value_format(attribute_or_empty(node, "format"));
    scaling.supplied.format = static_cast<bool>(node.attribute("format"));
    scaling.minimum = attribute_or_empty(node, "min");
    scaling.maximum = attribute_or_empty(node, "max");
    scaling.coarse_increment = attribute_or_empty(node, "inc");
    scaling.fine_increment = fine_increment_from(scaling.coarse_increment);
    scaling.storage_type = attribute_or_empty(node, "storagetype");
    scaling.endian = attribute_or_empty(node, "endian");
    append_selections(node, scaling);
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
            "element <table> type '" + axis.type + "' attribute 'name'",
            "missing or empty axis name",
            definition_id);
    }
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
        if (!size.has_value())
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
            scaling_node, axis.scaling_name.empty() ? axis.name : axis.scaling_name);
        axis.scaling_name = scaling.name;
        axis.units = scaling.units;
        axis.format = scaling.format;
        axis.from_byte = scaling.from_byte;
        axis.to_byte = scaling.to_byte;
        axis.supplied.from_byte = static_cast<bool>(scaling_node.attribute("toexpr"));
        axis.supplied.to_byte = static_cast<bool>(scaling_node.attribute("frexpr"));
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
        return invalid(source, "element <table> attribute 'name'", "missing or empty map name", definition_id);
    }
    const bool is_top_level_x_axis = map.type == "X Axis";
    const bool is_top_level_y_axis = map.type == "Y Axis";
    auto address = optional_address(table, source, definition_id);
    if (!address)
    {
        return std::unexpected(address.error());
    }
    map.address = *address;

    if (is_top_level_x_axis || is_top_level_y_axis)
    {
        auto elements = dimension_attribute(table, "elements", 1, source, definition_id);
        if (!elements.has_value())
        {
            return std::unexpected(elements.error());
        }
        map.type = "2D";
        map.x_size = is_top_level_x_axis ? *elements : 1;
        map.y_size = is_top_level_y_axis ? *elements : 1;
        map.supplied.x_size =
            is_top_level_x_axis && static_cast<bool>(table.attribute("elements"));
        map.supplied.y_size =
            is_top_level_y_axis && static_cast<bool>(table.attribute("elements"));
    }
    else
    {
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
    }

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
    if (const pugi::xml_node scaling_node = table.child("scaling"))
    {
        Scaling scaling = parse_scaling(
            scaling_node, map.scaling_name.empty() ? map.id : map.scaling_name);
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
        const std::string type = attribute_or_empty(axis_table, "type");
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
                const bool dimension_supplied = map.supplied.y_size;
                map.x_size = map.y_size;
                map.y_size = 1;
                map.supplied.x_size = dimension_supplied;
                map.supplied.y_size = dimension_supplied;
            }
            auto axis = parse_axis(axis_table, map.x_size, source, definition_id, scalings);
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
            auto axis = parse_axis(axis_table, map.y_size, source, definition_id, scalings);
            if (!axis)
            {
                return std::unexpected(axis.error());
            }
            map.y_axis = std::move(*axis);
        }
    }
    if (!map.x_axis.supplied.tracked && table.child("data"))
    {
        map.x_axis.supplied.tracked = true;
        map.x_axis.type = "Static X Axis";
        for (pugi::xml_node data : table.children("data"))
        {
            map.x_axis.static_data.push_back(trim_copy(data.child_value()));
        }
        map.x_axis.supplied.static_data = true;
        map.x_axis.size =
            static_cast<std::uint32_t>(map.x_axis.static_data.size());
        map.x_axis.supplied.size = true;
        map.x_size = map.x_axis.size;
        map.y_size = 1;
        map.supplied.x_size = true;
        map.supplied.y_size = true;
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
    auto identity = parse_identity(rom_id, source, std::move(*definition_id));
    if (!identity)
    {
        return std::unexpected(identity.error());
    }

    return std::vector<DefinitionIndexEntry>{DefinitionIndexEntry{
        .format = DefinitionFormat::EcuFlash,
        .definition_id = std::move(identity->xml_id),
        .internal_id = std::move(identity->internal_id),
        .internal_id_address = identity->internal_id_address,
        .internal_id_encoding = IdEncoding::AsciiOrHex,
        .ecu_id = std::move(identity->ecu_id),
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
    auto identity = parse_identity(rom_id, source, std::move(*definition_id));
    if (!identity)
    {
        return std::unexpected(identity.error());
    }

    UnresolvedDefinition definition{
        .format = DefinitionFormat::EcuFlash,
        .source = std::string(source),
        .identity = std::move(*identity),
        .metadata = parse_metadata(rom_id),
        .parents = parent_references(*root),
    };

    std::unordered_map<std::string, Scaling> global_scalings;
    for (pugi::xml_node scaling_node : root->children("scaling"))
    {
        Scaling scaling = parse_scaling(scaling_node, {});
        if (!scaling.name.empty())
        {
            const auto [existing, inserted] = global_scalings.try_emplace(scaling.name, scaling);
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
