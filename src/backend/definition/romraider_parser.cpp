#include "src/backend/definition/romraider_parser.h"

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

namespace fastecu::definition
{
namespace
{

std::string trim_copy(std::string_view value)
{
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
    {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
    {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string detail_prefix(std::string_view source, std::string_view definition_id = {})
{
    std::string detail = "RomRaider source '" + std::string(source) + "'";
    if (!definition_id.empty())
    {
        detail += ", definition '" + std::string(definition_id) + "'";
    }
    detail += ": ";
    return detail;
}

std::unexpected<Error> invalid(
    std::string_view source,
    std::string context,
    std::string message,
    std::string_view definition_id = {})
{
    return fail(
        ErrorKind::InvalidConfig,
        detail_prefix(source, definition_id) + std::move(context) + ": " + std::move(message));
}

Result<pugi::xml_node> parse_root(
    pugi::xml_document& document,
    std::span<const std::uint8_t> xml,
    std::string_view source)
{
    const pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size());
    if (!parsed)
    {
        return invalid(
            source,
            "XML document",
            std::string("malformed XML: ") + parsed.description());
    }

    const pugi::xml_node root = document.document_element();
    if (!root || std::string_view(root.name()) != "roms")
    {
        const std::string actual = root ? std::string("<") + root.name() + ">" : "no root element";
        return invalid(
            source,
            "root element <roms>",
            "wrong root; found " + actual);
    }
    return root;
}

std::string child_text(pugi::xml_node parent, const char *child_name)
{
    const pugi::xml_node child = parent.child(child_name);
    return child ? trim_copy(child.child_value()) : std::string{};
}

Result<std::string> required_child_text(
    pugi::xml_node parent,
    const char *parent_name,
    const char *child_name,
    std::string_view source)
{
    const std::string value = child_text(parent, child_name);
    if (value.empty())
    {
        return invalid(
            source,
            std::string("element <") + parent_name + "> child <" + child_name + ">",
            "missing or empty required text");
    }
    return value;
}

Result<std::uint64_t> parse_hex_unsigned(
    std::string_view value,
    std::string_view source,
    std::string context,
    std::string_view definition_id)
{
    std::string trimmed = trim_copy(value);
    std::string_view digits = trimmed;
    if (digits.starts_with("0x") || digits.starts_with("0X"))
    {
        digits.remove_prefix(2);
    }
    if (digits.empty() || digits.front() == '+' || digits.front() == '-')
    {
        return invalid(
            source,
            std::move(context),
            "invalid hexadecimal unsigned value '" + trimmed + "'",
            definition_id);
    }

    std::uint64_t parsed = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), parsed, 16);
    if (error != std::errc{} || end != digits.data() + digits.size())
    {
        return invalid(
            source,
            std::move(context),
            "invalid hexadecimal unsigned value '" + trimmed + "'",
            definition_id);
    }
    return parsed;
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
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() || parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max())
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

std::string attribute_or_empty(pugi::xml_node node, const char *name)
{
    return trim_copy(node.attribute(name).value());
}

std::string selection_name(std::string name)
{
    if (name == "on")
    {
        return "enabled";
    }
    if (name == "off")
    {
        return "disabled";
    }
    return name;
}

void append_selections(pugi::xml_node parent, Scaling& scaling)
{
    for (pugi::xml_node state : parent.children("state"))
    {
        scaling.selections.emplace_back(
            selection_name(attribute_or_empty(state, "name")),
            attribute_or_empty(state, "data"));
    }
    for (pugi::xml_node data : parent.children("data"))
    {
        std::string value = attribute_or_empty(data, "value");
        if (value.empty())
        {
            value = attribute_or_empty(data, "data");
        }
        scaling.selections.emplace_back(
            selection_name(attribute_or_empty(data, "name")),
            std::move(value));
    }
}

Scaling parse_scaling(
    pugi::xml_node scaling_node,
    std::string fallback_name,
    pugi::xml_node owner)
{
    Scaling scaling;
    scaling.name = attribute_or_empty(scaling_node, "name");
    if (scaling.name.empty())
    {
        scaling.name = std::move(fallback_name);
    }
    scaling.units = attribute_or_empty(scaling_node, "units");
    scaling.from_byte = attribute_or_empty(scaling_node, "expression");
    if (scaling.from_byte.empty())
    {
        scaling.from_byte = "x";
    }
    scaling.to_byte = attribute_or_empty(scaling_node, "to_byte");
    if (scaling.to_byte.empty())
    {
        scaling.to_byte = "x";
    }
    scaling.format = attribute_or_empty(scaling_node, "format");
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
    axis.type = attribute_or_empty(table, "type");
    axis.name = attribute_or_empty(table, "name");
    if (axis.name.empty())
    {
        return invalid(
            source,
            "element <table> attribute 'name'",
            "missing or empty axis name",
            definition_id);
    }
    axis.storage_type = attribute_or_empty(table, "storagetype");
    axis.endian = attribute_or_empty(table, "endian");

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

    axis.scaling_name = attribute_or_empty(table, "scaling");
    const pugi::xml_node scaling_node = table.child("scaling");
    if (scaling_node)
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
    map.id = attribute_or_empty(table, "id");
    map.name = attribute_or_empty(table, "name");
    if (map.name.empty())
    {
        return invalid(
            source,
            "element <table> attribute 'name'",
            "missing or empty map name",
            definition_id);
    }
    if (map.id.empty())
    {
        map.id = map.name;
    }

    map.type = attribute_or_empty(table, "type");
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

    auto address = optional_address(table, source, definition_id);
    if (!address)
    {
        return std::unexpected(address.error());
    }
    map.address = *address;

    auto x_size = dimension_attribute(table, "sizex", 1, source, definition_id);
    if (!x_size)
    {
        return std::unexpected(x_size.error());
    }
    map.x_size = *x_size;

    auto y_size = dimension_attribute(table, "sizey", 1, source, definition_id);
    if (!y_size)
    {
        return std::unexpected(y_size.error());
    }
    map.y_size = *y_size;

    auto swap_xy = strict_boolean_attribute(table, "swapxy", source, definition_id);
    if (!swap_xy)
    {
        return std::unexpected(swap_xy.error());
    }
    map.swap_xy = *swap_xy;

    auto flip_x = strict_boolean_attribute(table, "flipx", source, definition_id);
    if (!flip_x)
    {
        return std::unexpected(flip_x.error());
    }
    map.flip_x = *flip_x;

    auto flip_y = strict_boolean_attribute(table, "flipy", source, definition_id);
    if (!flip_y)
    {
        return std::unexpected(flip_y.error());
    }
    map.flip_y = *flip_y;

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

    for (pugi::xml_node axis_table : table.children("table"))
    {
        const std::string type = attribute_or_empty(axis_table, "type");
        if (type == "X Axis" || type == "Static X Axis" || type == "Static Y Axis" || (type == "Y Axis" && map.type == "2D"))
        {
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

Result<std::string> definition_id_for_rom(pugi::xml_node rom, std::string_view source)
{
    const pugi::xml_node rom_id = rom.child("romid");
    if (!rom_id)
    {
        return invalid(
            source,
            "element <rom> child <romid>",
            "missing required identity element");
    }
    return required_child_text(rom_id, "romid", "xmlid", source);
}

std::vector<std::string> parent_references(pugi::xml_node rom)
{
    const std::string parent = attribute_or_empty(rom, "base");
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
    auto root = parse_root(document, xml, source);
    if (!root)
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
            .source = std::string(source),
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
    auto root = parse_root(document, xml, source);
    if (!root)
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
        .source = std::string(source),
        .identity =
            RomIdentity{
                .xml_id = std::string(definition_id),
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
        if (!map_ids.insert(map->id).second)
        {
            return invalid(
                source,
                "element <table> attribute 'id' or 'name'",
                "duplicate map identity '" + map->id + "'",
                definition_id);
        }
        definition.maps.push_back(std::move(*map));
    }
    return definition;
}

} // namespace fastecu::definition
