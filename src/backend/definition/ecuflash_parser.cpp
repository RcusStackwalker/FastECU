#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/definition/parser_utils.h"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <format>
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

Result<UnresolvedScaling> parse_scaling(
    pugi::xml_node node,
    std::string fallback_name,
    std::string_view source,
    std::string_view definition_id)
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
    auto storage_type = optional_storage_type_attribute(node, "storagetype", source, definition_id);
    if (!storage_type.has_value())
    {
        return std::unexpected(storage_type.error());
    }
    scaling.storage_type = *storage_type;
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
    if (auto status = populate_common_axis_attributes(table, axis, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (axis.name.empty())
    {
        return invalid(
            source,
            std::format("element <table> type '{}' attribute 'name'", axis.type),
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
        if (!size)
        {
            return std::unexpected(size.error());
        }
        axis.size = *size;
    }
    axis.scaling_name = value_or_empty(table.attribute("scaling"));
    if (const pugi::xml_node scaling_node = table.child("scaling"))
    {
        auto scaling = parse_scaling(
            scaling_node, axis.scaling_name.empty() ? axis.name : axis.scaling_name, source, definition_id);
        if (!scaling.has_value())
        {
            return std::unexpected(scaling.error());
        }
        apply_scaling_to_axis(*scaling, axis);
        scalings.push_back(std::move(*scaling));
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
    if (auto status = populate_common_map_attributes(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
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
        if (auto status = populate_optional_dimension(
                table, "sizex", map.x_size, source, definition_id);
            !status)
        {
            return std::unexpected(status.error());
        }
        if (auto status = populate_optional_dimension(
                table, "sizey", map.y_size, source, definition_id);
            !status)
        {
            return std::unexpected(status.error());
        }
    }

    if (auto status = populate_optional_boolean(
            table, "swapxy", map.swap_xy, source, definition_id);
        !status)
    {
        return std::unexpected(status.error());
    }
    if (auto status = populate_optional_boolean(
            table, "flipx", map.flip_x, source, definition_id);
        !status)
    {
        return std::unexpected(status.error());
    }
    if (auto status = populate_optional_boolean(
            table, "flipy", map.flip_y, source, definition_id);
        !status)
    {
        return std::unexpected(status.error());
    }

    map.scaling_name = value_or_empty(table.attribute("scaling"));
    if (const pugi::xml_node scaling_node = table.child("scaling"))
    {
        auto parsed_scaling = parse_scaling(
            scaling_node,
            map.scaling_name.empty() ? map.id.value_or(map.name) : map.scaling_name,
            source,
            definition_id);
        if (!parsed_scaling.has_value())
        {
            return std::unexpected(parsed_scaling.error());
        }
        UnresolvedScaling& scaling = *parsed_scaling;
        map.scaling_name = scaling.name;
        if (!map.storage_type)
        {
            map.storage_type = scaling.storage_type;
        }
        if (map.endian.empty())
        {
            map.endian = scaling.endian;
        }
        if (scaling.storage_type == StorageType::Bloblist)
        {
            map.type = "Selectable";
        }
        scalings.push_back(std::move(scaling));
    }

    if (auto status =
            populate_axes(table, map, source, definition_id, scalings, parse_axis);
        !status)
    {
        return std::unexpected(status.error());
    }
    if (map.x_axis == UnresolvedAxisDefinition{} && table.child("data"))
    {
        map.x_axis.type = "Static X Axis";
        std::vector<std::string> values;
        for (pugi::xml_node data : table.children("data"))
        {
            values.push_back(trim_copy(data.child_value()));
        }
        map.x_axis.static_data = std::move(values);
        map.x_axis.size =
            static_cast<std::uint32_t>(map.x_axis.static_data->size());
        map.x_size = map.x_axis.size;
        map.y_size = 1;
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

struct ParsedHeader
{
    pugi::xml_node root;
    pugi::xml_node rom_id;
    std::string definition_id;
    std::optional<std::uint64_t> internal_id_address;
};

Result<ParsedHeader> parse_header(
    pugi::xml_document& document,
    std::span<const std::uint8_t> xml,
    std::string_view source)
{
    auto root = parse_root(document, xml, source, "rom"sv);
    if (!root)
    {
        return std::unexpected(root.error());
    }
    auto definition_id = definition_id_for_rom(*root, source);
    if (!definition_id)
    {
        return std::unexpected(definition_id.error());
    }
    const pugi::xml_node rom_id = root->child("romid");
    auto internal_id_address =
        optional_hex_element(rom_id, "internalidaddress", source, *definition_id);
    if (!internal_id_address)
    {
        return std::unexpected(internal_id_address.error());
    }
    return ParsedHeader{
        .root = *root,
        .rom_id = rom_id,
        .definition_id = std::move(*definition_id),
        .internal_id_address = *internal_id_address,
    };
}

} // namespace

Result<std::vector<DefinitionIndexEntry>> parse_ecuflash_index(
    std::span<const std::uint8_t> xml, std::string_view source)
{
    pugi::xml_document document;
    auto header = parse_header(document, xml, source);
    if (!header)
    {
        return std::unexpected(header.error());
    }

    return std::vector<DefinitionIndexEntry>{DefinitionIndexEntry{
        .format = DefinitionFormat::EcuFlash,
        .definition_id = std::move(header->definition_id),
        .internal_id = child_text(header->rom_id, "internalidstring"),
        .internal_id_address = header->internal_id_address,
        .internal_id_encoding = IdEncoding::AsciiOrHex,
        .ecu_id = child_text(header->rom_id, "ecuid"),
        .source = std::string(source),
        .parents = parent_references(header->root),
    }};
}

Result<UnresolvedDefinition> parse_ecuflash_definition(
    std::span<const std::uint8_t> xml, std::string_view source)
{
    pugi::xml_document document;
    auto header = parse_header(document, xml, source);
    if (!header)
    {
        return std::unexpected(header.error());
    }

    UnresolvedDefinition definition{
        .format = DefinitionFormat::EcuFlash,
        .source = std::string(source),
        .identity = RomIdentity{
            .xml_id = std::move(header->definition_id),
            .internal_id = child_text(header->rom_id, "internalidstring"),
            .ecu_id = child_text(header->rom_id, "ecuid"),
            .internal_id_address = header->internal_id_address,
        },
        .metadata = parse_metadata(header->rom_id),
        .parents = parent_references(header->root),
    };

    std::unordered_map<std::string, UnresolvedScaling> global_scalings;
    for (pugi::xml_node scaling_node : header->root.children("scaling"))
    {
        auto parsed_scaling = parse_scaling(scaling_node, {}, source, definition.identity.xml_id);
        if (!parsed_scaling.has_value())
        {
            return std::unexpected(parsed_scaling.error());
        }
        UnresolvedScaling& scaling = *parsed_scaling;
        if (!scaling.name.empty())
        {
            const auto [existing, inserted] = global_scalings.emplace(scaling.name, scaling);
            if (!inserted && existing->second != scaling)
            {
                return invalid(
                    source,
                    "element <scaling> attribute 'name'",
                    std::format("conflicting duplicate global scaling '{}'", scaling.name),
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
    for (pugi::xml_node table : header->root.children("table"))
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
                std::format("duplicate map identity '{}'", map_id),
                definition.identity.xml_id);
        }
        definition.maps.push_back(std::move(*map));
    }
    return definition;
}

} // namespace fastecu::definition
