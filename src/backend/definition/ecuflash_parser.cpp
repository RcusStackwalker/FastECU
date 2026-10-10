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

std::string ConvertValueFormat(std::string_view format)
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

std::string FineIncrementFrom(std::string_view coarse_increment)
{
    const std::string value = TrimCopy(coarse_increment);
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

void AppendSelections(pugi::xml_node parent, UnresolvedScaling& scaling)
{
    for (pugi::xml_node data : parent.children("data"))
    {
        std::string value = ValueOrEmpty(data.attribute("value"));
        if (value.empty())
        {
            value = ValueOrEmpty(data.attribute("data"));
        }
        scaling.selections.emplace_back(SelectionName(ValueOrEmpty(data.attribute("name"))), std::move(value));
    }
}

Result<UnresolvedScaling> ParseScaling(pugi::xml_node node, std::string fallback_name, std::string_view source,
                                       std::string_view definition_id)
{
    UnresolvedScaling scaling;
    scaling.name = ValueOrEmpty(node.attribute("name"));
    if (scaling.name.empty())
    {
        scaling.name = std::move(fallback_name);
    }
    scaling.units = ValueOrEmpty(node.attribute("units"));
    if (const auto toexpr = node.attribute("toexpr"))
    {
        scaling.from_byte = ValueOrEmpty(toexpr);
    }
    if (const auto frexpr = node.attribute("frexpr"))
    {
        scaling.to_byte = ValueOrEmpty(frexpr);
    }
    if (const auto format = node.attribute("format"))
    {
        scaling.format = ConvertValueFormat(ValueOrEmpty(format));
    }
    scaling.minimum = ValueOrEmpty(node.attribute("min"));
    scaling.maximum = ValueOrEmpty(node.attribute("max"));
    scaling.coarse_increment = ValueOrEmpty(node.attribute("inc"));
    scaling.fine_increment = FineIncrementFrom(scaling.coarse_increment);
    auto storage_type = OptionalStorageTypeAttribute(node, "storagetype", source, definition_id);
    if (!storage_type.has_value())
    {
        return std::unexpected(storage_type.error());
    }
    scaling.storage_type = *storage_type;
    scaling.endian = ValueOrEmpty(node.attribute("endian"));
    AppendSelections(node, scaling);
    return scaling;
}

Result<UnresolvedAxisDefinition> ParseAxis(pugi::xml_node table, std::uint32_t default_size, std::string_view source,
                                           std::string_view definition_id, std::vector<UnresolvedScaling>& scalings)
{
    return ParseAxisDefinition(table, default_size, source, definition_id, scalings,
                               [&](pugi::xml_node scaling_node, std::string fallback_name)
                               { return ParseScaling(scaling_node, std::move(fallback_name), source, definition_id); });
}

Result<UnresolvedCalibrationMap> ParseTable(pugi::xml_node table, std::string_view source,
                                            std::string_view definition_id, std::vector<UnresolvedScaling>& scalings)
{
    UnresolvedCalibrationMap map;
    if (auto status = PopulateMapHeader(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }

    // A top-level axis table is a 2D map sized by its own element count.
    if (const bool is_x_axis = map.type == "X Axis"; is_x_axis || map.type == "Y Axis")
    {
        auto elements = DimensionAttribute(table, "elements", 1, source, definition_id);
        if (!elements.has_value())
        {
            return std::unexpected(elements.error());
        }
        map.type = "2D";
        map.x_size = is_x_axis ? *elements : 1;
        map.y_size = is_x_axis ? 1 : *elements;
    }
    else if (auto status = PopulateMapSize(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = PopulateMapOrientation(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }

    auto scaling =
        ParseMapScaling(table, map, [&](pugi::xml_node scaling_node, std::string fallback_name)
                        { return ParseScaling(scaling_node, std::move(fallback_name), source, definition_id); });
    if (!scaling.has_value())
    {
        return std::unexpected(scaling.error());
    }
    if (scaling->has_value())
    {
        if ((*scaling)->storage_type == StorageType::kBloblist)
        {
            map.type = "Selectable";
        }
        scalings.push_back(std::move(**scaling));
    }

    if (auto status = PopulateAxes(table, map, source, definition_id, scalings, ParseAxis); !status.has_value())
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
            values.push_back(TableElementText(data));
        }
        map.x_axis.static_data = std::move(values);
        map.x_axis.size = static_cast<std::uint32_t>(map.x_axis.static_data->size());
        map.x_size = map.x_axis.size;
        map.y_size = 1;
    }
    return map;
}

std::vector<std::string> ParentReferences(pugi::xml_node rom)
{
    std::vector<std::string> parents;
    for (pugi::xml_node include : rom.children("include"))
    {
        const std::string parent{TrimHeaderText(ReadElementText(include))};
        if (!parent.empty())
        {
            parents.push_back(parent);
        }
    }
    return parents;
}

Result<ParsedRomHeader> ParseHeader(pugi::xml_document& document, std::span<const std::uint8_t> xml,
                                    std::string_view source)
{
    const auto root = ParseRoot(document, xml, source, "rom"sv);
    if (!root.has_value())
    {
        return std::unexpected(root.error());
    }
    return ParseRomHeader(*root, source);
}

} // namespace

Result<std::vector<DefinitionIndexEntry>> ParseEcuflashIndex(std::span<const std::uint8_t> xml, std::string_view source)
{
    pugi::xml_document document;
    auto header = ParseHeader(document, xml, source);
    if (!header.has_value())
    {
        return std::unexpected(header.error());
    }

    return std::vector<DefinitionIndexEntry>{DefinitionIndexEntry{
        .format = DefinitionFormat::kEcuFlash,
        .definition_id = std::move(header->identity.xml_id),
        .internal_id = std::move(header->identity.internal_id),
        .internal_id_address = header->identity.internal_id_address,
        .internal_id_encoding = IdEncoding::kAsciiOrHex,
        .ecu_id = std::move(header->identity.ecu_id),
        .flash_method = HeaderChildText(header->rom.child("romid"), "flashmethod"),
        .source = std::string(source),
        .parents = ParentReferences(header->rom),
    }};
}

Result<UnresolvedDefinition> ParseEcuflashDefinition(std::span<const std::uint8_t> xml, std::string_view source)
{
    pugi::xml_document document;
    auto header = ParseHeader(document, xml, source);
    if (!header.has_value())
    {
        return std::unexpected(header.error());
    }

    UnresolvedDefinition definition{
        .format = DefinitionFormat::kEcuFlash,
        .source = std::string(source),
        .identity = std::move(header->identity),
        .metadata = ParseMetadata(header->rom.child("romid")),
        .parents = ParentReferences(header->rom),
    };

    std::unordered_map<std::string, UnresolvedScaling> global_scalings;
    for (pugi::xml_node scaling_node : header->rom.children("scaling"))
    {
        auto parsed_scaling = ParseScaling(scaling_node, {}, source, definition.identity.xml_id);
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
                return Invalid(source, "element <scaling> attribute 'name'",
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
        auto map = ParseTable(table, source, definition.identity.xml_id, definition.scalings);
        if (!map)
        {
            return std::unexpected(map.error());
        }
        if (const std::string map_id = map->id.value_or(map->name); !map_ids.insert(map_id).second)
        {
            return Invalid(source, "element <table> attribute 'id' or 'name'",
                           std::format("duplicate map identity '{}'", map_id), definition.identity.xml_id);
        }
        definition.maps.push_back(std::move(*map));
    }
    return definition;
}

} // namespace fastecu::definition
