#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/definition/parser_utils.h"
#include "src/backend/definition/text_format.h"

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

Result<UnresolvedScaling> parse_scaling(pugi::xml_node node, std::string fallback_name, std::string_view source,
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

Result<UnresolvedAxisDefinition> parse_axis(pugi::xml_node table, std::uint32_t default_size, std::string_view source,
                                            std::string_view definition_id, std::vector<UnresolvedScaling>& scalings)
{
    return parse_axis_definition(
        table, default_size, source, definition_id, scalings,
        [&](pugi::xml_node scaling_node, std::string fallback_name)
        { return parse_scaling(scaling_node, std::move(fallback_name), source, definition_id); });
}

Result<UnresolvedCalibrationMap> parse_table(pugi::xml_node table, std::string_view source,
                                             std::string_view definition_id, std::vector<UnresolvedScaling>& scalings)
{
    UnresolvedCalibrationMap map;
    if (auto status = populate_map_header(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }

    // A top-level axis table is a 2D map sized by its own element count.
    if (const bool is_x_axis = map.type == "X Axis"; is_x_axis || map.type == "Y Axis")
    {
        auto elements = dimension_attribute(table, "elements", 1, source, definition_id);
        if (!elements.has_value())
        {
            return std::unexpected(elements.error());
        }
        map.type = "2D";
        map.x_size = is_x_axis ? *elements : 1;
        map.y_size = is_x_axis ? 1 : *elements;
    }
    else if (auto status = populate_map_size(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = populate_map_orientation(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }

    auto scaling =
        parse_map_scaling(table, map, [&](pugi::xml_node scaling_node, std::string fallback_name)
                          { return parse_scaling(scaling_node, std::move(fallback_name), source, definition_id); });
    if (!scaling.has_value())
    {
        return std::unexpected(scaling.error());
    }
    if (scaling->has_value())
    {
        if ((*scaling)->storage_type == StorageType::Bloblist)
        {
            map.type = "Selectable";
        }
        scalings.push_back(std::move(**scaling));
    }

    if (auto status = populate_axes(table, map, source, definition_id, scalings, parse_axis); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    // A map with no axis but bare <data> children carries its X axis values inline.
    if (map.x_axis == UnresolvedAxisDefinition{} && table.child("data"))
    {
        map.x_axis.type = "Static X Axis";
        std::vector<std::string> values;
        for (pugi::xml_node data : table.children("data"))
        {
            values.push_back(table_element_text(data));
        }
        map.x_axis.static_data = std::move(values);
        map.x_axis.size = static_cast<std::uint32_t>(map.x_axis.static_data->size());
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
        const std::string parent{trim_header_text(read_element_text(include))};
        if (!parent.empty())
        {
            parents.push_back(parent);
        }
    }
    return parents;
}

Result<ParsedRomHeader> parse_header(pugi::xml_document& document, std::span<const std::uint8_t> xml,
                                     std::string_view source)
{
    const auto root = parse_root(document, xml, source, "rom"sv);
    if (!root.has_value())
    {
        return std::unexpected(root.error());
    }
    return parse_rom_header(*root, source);
}

} // namespace

Result<std::vector<DefinitionIndexEntry>> parse_ecuflash_index(std::span<const std::uint8_t> xml,
                                                               std::string_view source)
{
    pugi::xml_document document;
    auto header = parse_header(document, xml, source);
    if (!header.has_value())
    {
        return std::unexpected(header.error());
    }

    return std::vector<DefinitionIndexEntry>{DefinitionIndexEntry{
        .format = DefinitionFormat::EcuFlash,
        .definition_id = std::move(header->identity.xml_id),
        .internal_id = std::move(header->identity.internal_id),
        .internal_id_address = header->identity.internal_id_address,
        .internal_id_encoding = IdEncoding::AsciiOrHex,
        .ecu_id = std::move(header->identity.ecu_id),
        .source = std::string(source),
        .parents = parent_references(header->rom),
    }};
}

Result<UnresolvedDefinition> parse_ecuflash_definition(std::span<const std::uint8_t> xml, std::string_view source)
{
    pugi::xml_document document;
    auto header = parse_header(document, xml, source);
    if (!header.has_value())
    {
        return std::unexpected(header.error());
    }

    UnresolvedDefinition definition{
        .format = DefinitionFormat::EcuFlash,
        .source = std::string(source),
        .identity = std::move(header->identity),
        .metadata = parse_metadata(header->rom.child("romid")),
        .parents = parent_references(header->rom),
    };

    std::unordered_map<std::string, UnresolvedScaling> global_scalings;
    for (pugi::xml_node scaling_node : header->rom.children("scaling"))
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
                return invalid(source, "element <scaling> attribute 'name'",
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
    for (pugi::xml_node table : header->rom.children("table"))
    {
        auto map = parse_table(table, source, definition.identity.xml_id, definition.scalings);
        if (!map)
        {
            return std::unexpected(map.error());
        }
        if (const std::string map_id = map->id.value_or(map->name); !map_ids.insert(map_id).second)
        {
            return invalid(source, "element <table> attribute 'id' or 'name'",
                           std::format("duplicate map identity '{}'", map_id), definition.identity.xml_id);
        }
        definition.maps.push_back(std::move(*map));
    }
    return definition;
}

} // namespace fastecu::definition
