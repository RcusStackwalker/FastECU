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
                std::string("element <romid> child <") +
                    child_name + ">",
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

Result<std::optional<std::uint64_t>> optional_hex_attribute(
    pugi::xml_node node,
    const char *attribute_name,
    std::string_view source,
    std::string_view definition_id)
{
    const pugi::xml_attribute attribute = node.attribute(attribute_name);
    if (!attribute)
    {
        return std::optional<std::uint64_t>{};
    }

    auto parsed = parse_hex_unsigned(
        attribute.value(),
        source,
        std::string("element <") + node.name() + "> attribute '" + attribute_name + "'",
        definition_id);
    if (!parsed)
    {
        return std::unexpected(parsed.error());
    }
    return std::optional<std::uint64_t>{*parsed};
}

Result<std::optional<std::uint64_t>> optional_hex_element(
    pugi::xml_node parent,
    const char *child_name,
    std::string_view source,
    std::string_view definition_id)
{
    const pugi::xml_node child = parent.child(child_name);
    if (!child)
    {
        return std::optional<std::uint64_t>{};
    }

    auto parsed = parse_hex_unsigned(
        child.child_value(),
        source,
        std::string("element <") + parent.name() + "> child <" + child_name + ">",
        definition_id);
    if (!parsed)
    {
        return std::unexpected(parsed.error());
    }
    return std::optional<std::uint64_t>{*parsed};
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

Result<std::uint32_t> dimension_attribute(
    pugi::xml_node node,
    const char *attribute_name,
    std::uint32_t default_value,
    std::string_view source,
    std::string_view definition_id)
{
    const pugi::xml_attribute attribute = node.attribute(attribute_name);
    if (!attribute)
    {
        return default_value;
    }

    const std::string value = trim_copy(attribute.value());
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max())
    {
        return invalid(
            source,
            std::string("element <") + node.name() + "> attribute '" + attribute_name + "'",
            "invalid positive dimension '" + value + "'",
            definition_id);
    }
    return static_cast<std::uint32_t>(parsed);
}

Result<bool> strict_boolean_attribute(
    pugi::xml_node node,
    const char *attribute_name,
    std::string_view source,
    std::string_view definition_id)
{
    const pugi::xml_attribute attribute = node.attribute(attribute_name);
    if (!attribute)
    {
        return false;
    }
    const std::string_view value = attribute.value();
    if (value == "true")
    {
        return true;
    }
    if (value == "false")
    {
        return false;
    }
    return invalid(
        source,
        std::string("element <") + node.name() + "> attribute '" + attribute_name + "'",
        "invalid strict boolean '" + std::string(value) + "'; expected 'true' or 'false'",
        definition_id);
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
    for (pugi::xml_node data : parent.children("data"))
    {
        std::string value = attribute_or_empty(data, "value");
        if (value.empty())
        {
            value = attribute_or_empty(data, "data");
        }
        scaling.selections.emplace_back(selection_name(attribute_or_empty(data, "name")), std::move(value));
    }
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
    axis.supplied.tracked = true;
    axis.type = attribute_or_empty(table, "type");
    axis.name = attribute_or_empty(table, "name");
    if (axis.name.empty())
    {
        return invalid(
            source,
            "element <table> type '" + axis.type + "' attribute 'name'",
            "missing or empty axis name",
            definition_id);
    }
    axis.storage_type = attribute_or_empty(table, "storagetype");
    axis.endian = attribute_or_empty(table, "endian");
    if (table.attribute("startpos"))
    {
        axis.start_position = attribute_or_empty(table, "startpos");
        axis.supplied.start_position = true;
    }
    if (table.attribute("interval"))
    {
        axis.interval = attribute_or_empty(table, "interval");
        axis.supplied.interval = true;
    }
    for (pugi::xml_node data : table.children("data"))
    {
        axis.static_data.push_back(trim_copy(data.child_value()));
        axis.supplied.static_data = true;
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
    map.supplied.tracked = true;
    map.id = attribute_or_empty(table, "id");
    map.supplied.stable_id = !map.id.empty();
    map.name = attribute_or_empty(table, "name");
    if (map.name.empty())
    {
        return invalid(source, "element <table> attribute 'name'", "missing or empty map name", definition_id);
    }
    if (map.id.empty())
    {
        map.id = map.name;
    }
    map.type = attribute_or_empty(table, "type");
    const bool is_top_level_x_axis = map.type == "X Axis";
    const bool is_top_level_y_axis = map.type == "Y Axis";
    map.category = attribute_or_empty(table, "category");
    map.subcategory = attribute_or_empty(table, "subcategory");
    map.description = attribute_or_empty(table, "description");
    if (map.description.empty())
    {
        map.description = child_text(table, "description");
    }
    map.level = attribute_or_empty(table, "level");
    map.user_level = attribute_or_empty(table, "userlevel");
    map.storage_type = attribute_or_empty(table, "storagetype");
    map.endian = attribute_or_empty(table, "endian");
    if (table.attribute("startpos"))
    {
        map.start_position = attribute_or_empty(table, "startpos");
        map.supplied.start_position = true;
    }
    if (table.attribute("interval"))
    {
        map.interval = attribute_or_empty(table, "interval");
        map.supplied.interval = true;
    }

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
        map.supplied.x_size =
            is_top_level_x_axis && static_cast<bool>(table.attribute("elements"));
        map.supplied.y_size =
            is_top_level_y_axis && static_cast<bool>(table.attribute("elements"));
    }
    else
    {
        auto x_size = dimension_attribute(table, "sizex", 1, source, definition_id);
        if (!x_size)
        {
            return std::unexpected(x_size.error());
        }
        map.x_size = *x_size;
        map.supplied.x_size = static_cast<bool>(table.attribute("sizex"));
        auto y_size = dimension_attribute(table, "sizey", 1, source, definition_id);
        if (!y_size)
        {
            return std::unexpected(y_size.error());
        }
        map.y_size = *y_size;
        map.supplied.y_size = static_cast<bool>(table.attribute("sizey"));
    }

    auto swap_xy = strict_boolean_attribute(table, "swapxy", source, definition_id);
    if (!swap_xy)
    {
        return std::unexpected(swap_xy.error());
    }
    map.swap_xy = *swap_xy;
    map.supplied.swap_xy = static_cast<bool>(table.attribute("swapxy"));
    auto flip_x = strict_boolean_attribute(table, "flipx", source, definition_id);
    if (!flip_x)
    {
        return std::unexpected(flip_x.error());
    }
    map.flip_x = *flip_x;
    map.supplied.flip_x = static_cast<bool>(table.attribute("flipx"));
    auto flip_y = strict_boolean_attribute(table, "flipy", source, definition_id);
    if (!flip_y)
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
        .internal_id_encoding = IdEncoding::AsciiOrHex,
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

    std::unordered_map<std::string, Scaling> global_scalings;
    for (pugi::xml_node scaling_node : root->children("scaling"))
    {
        Scaling scaling = parse_scaling(scaling_node, {});
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
        if (!map_ids.insert(map->id).second)
        {
            return invalid(
                source,
                "element <table> attribute 'id' or 'name'",
                "duplicate map identity '" + map->id + "'",
                definition.identity.xml_id);
        }
        definition.maps.push_back(std::move(*map));
    }
    return definition;
}

} // namespace fastecu::definition
