#pragma once

#include "src/backend/definition/definition_model.h"

#include <array>
#include <charconv>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>

#include <pugixml.hpp>

namespace fastecu::definition
{

inline std::string trim_copy(std::string_view value)
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

inline std::string detail_prefix(std::string_view source, std::string_view definition_id = {})
{
    std::string detail = "EcuFlash/RomRaider source '" + std::string(source) + "'";
    if (!definition_id.empty())
    {
        detail += ", definition '" + std::string(definition_id) + "'";
    }
    return detail + ": ";
}

inline std::unexpected<Error> invalid(
    std::string_view source,
    std::string context,
    std::string message,
    std::string_view definition_id = {})
{
    return fail(
        ErrorKind::InvalidConfig,
        detail_prefix(source, definition_id) + std::move(context) + ": " + std::move(message));
}

inline std::string child_text(pugi::xml_node parent, std::string_view child_name)
{
    const pugi::xml_node child = parent.child(child_name);
    return child ? trim_copy(child.child_value()) : std::string{};
}

inline Result<std::string> required_child_text(
    pugi::xml_node parent,
    std::string_view parent_name,
    std::string_view child_name,
    std::string_view source)
{
    const std::string value = child_text(parent, child_name);
    if (value.empty())
    {
        return invalid(
            source,
            std::format("element <{}> child <{}>", parent_name, child_name),
            "missing or empty required text");
    }
    return value;
}

inline Result<pugi::xml_node> parse_root(
    pugi::xml_document& document,
    std::span<const std::uint8_t> xml,
    std::string_view source,
    std::string_view root_name)
{
    if (const pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size()); !parsed)
    {
        return invalid(source, "XML document", std::string("malformed XML: ") + parsed.description());
    }

    const pugi::xml_node root = document.document_element();
    if (!root || root.name() != root_name)
    {
        const std::string actual = root ? std::format("<{}>", root.name()) : "no root element";
        return invalid(source, std::format("root element <{}>", root_name), "wrong root; found " + actual);
    }
    return root;
}

inline Result<std::uint64_t> parse_hex_unsigned(
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

    std::uint64_t parsed = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), parsed, 16);
    if (error != std::errc{} || end != digits.data() + digits.size())
    {
        return invalid(
            source,
            std::move(context),
            std::format("invalid hexadecimal unsigned value '{}'", trimmed),
            definition_id);
    }
    return parsed;
}

inline std::string attribute_or_empty(pugi::xml_node node, std::string_view name)
{
    return trim_copy(node.attribute(name).value());
}

inline std::string selection_name(std::string name)
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

inline Result<pugi::xml_node> identity_element(
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
                std::format("element <romid> child <{}>", child_name),
                "duplicate singleton identity element");
        }
    }
    return rom_id;
}

inline Result<std::string> definition_id_for_rom(
    pugi::xml_node rom,
    std::string_view source)
{
    auto rom_id = identity_element(rom, source);
    if (!rom_id)
    {
        return std::unexpected(rom_id.error());
    }
    return required_child_text(*rom_id, "romid", "xmlid", source);
}

inline RomMetadata parse_metadata(pugi::xml_node rom_id)
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

inline void append_data_selections(pugi::xml_node parent, Scaling& scaling)
{
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

inline void populate_common_axis_attributes(
    pugi::xml_node table,
    AxisDefinition& axis)
{
    axis.supplied.tracked = true;
    axis.type = attribute_or_empty(table, "type");
    axis.name = attribute_or_empty(table, "name");
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
}

inline void populate_common_map_attributes(
    pugi::xml_node table,
    CalibrationMap& map)
{
    map.supplied.tracked = true;
    map.id = attribute_or_empty(table, "id");
    map.supplied.stable_id = !map.id.empty();
    map.name = attribute_or_empty(table, "name");
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
}

inline Status append_unique_map(
    UnresolvedDefinition& definition,
    CalibrationMap map,
    std::unordered_set<std::string>& map_ids,
    std::string_view source)
{
    if (!map_ids.insert(map.id).second)
    {
        return invalid(
            source,
            "element <table> attribute 'id' or 'name'",
            "duplicate map identity '" + map.id + "'",
            definition.identity.xml_id);
    }
    definition.maps.push_back(std::move(map));
    return {};
}

inline Result<std::optional<std::uint64_t>> optional_hex_element(
    pugi::xml_node parent,
    std::string_view child_name,
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
        std::format("element <{}> child <{}>", parent.name(), child_name),
        definition_id);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    return std::optional<std::uint64_t>{*parsed};
}

inline Result<std::optional<std::uint64_t>> optional_hex_attribute(
    pugi::xml_node node,
    std::string_view attribute_name,
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
        std::format("element <{}> attribute '{}'", node.name(), attribute_name),
        definition_id);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    return std::optional<std::uint64_t>{*parsed};
}

inline Result<std::optional<std::uint64_t>> optional_address(
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

inline Result<RomIdentity> parse_identity(
    pugi::xml_node rom_id,
    std::string_view source,
    std::string definition_id)
{
    auto internal_id_address =
        optional_hex_element(rom_id, "internalidaddress", source, definition_id);
    if (!internal_id_address)
    {
        return std::unexpected(internal_id_address.error());
    }
    return RomIdentity{
        .xml_id = std::move(definition_id),
        .internal_id = child_text(rom_id, "internalidstring"),
        .ecu_id = child_text(rom_id, "ecuid"),
        .internal_id_address = *internal_id_address,
    };
}

inline Result<std::uint32_t> dimension_attribute(
    pugi::xml_node node,
    std::string_view attribute_name,
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
            std::format("element <{}> attribute '{}'", node.name(), attribute_name),
            std::format("invalid positive dimension '{}'", value),
            definition_id);
    }
    return static_cast<std::uint32_t>(parsed);
}

inline Result<bool> strict_boolean_attribute(
    pugi::xml_node node,
    std::string_view attribute_name,
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
        std::format("element <{}> attribute '{}'", node.name(), attribute_name),
        std::format("invalid strict boolean '{}'; expected 'true' or 'false'", value),
        definition_id);
}

} // namespace fastecu::definition
