#include "src/backend/definition/parser_utils.h"

#include "src/backend/definition/text_format.h"
#include "src/backend/definition/metadata_fields.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <format>
#include <limits>
#include <system_error>
#include <utility>

#include <pugixml.hpp>

namespace fastecu::definition
{

namespace
{
constexpr std::array kSingletonChildren{
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
} // namespace

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

std::string detail_prefix(std::string_view source, std::string_view definition_id)
{
    std::string detail = std::format("EcuFlash/RomRaider source '{}'", source);
    if (!definition_id.empty())
    {
        detail += std::format(", definition '{}'", definition_id);
    }
    return detail + ": ";
}

std::unexpected<Error> invalid(std::string_view source, std::string context, std::string message,
                               std::string_view definition_id)
{
    return fail(ErrorKind::InvalidConfig,
                std::format("{}{}: {}", detail_prefix(source, definition_id), context, message));
}

std::string read_element_text(pugi::xml_node element)
{
    std::string text;
    for (const auto child : element.children())
    {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata)
        {
            text.append(child.value());
        }
    }
    return text;
}

std::string header_child_text(pugi::xml_node parent, std::string_view name)
{
    const auto text = read_element_text(parent.child(name));
    return name == "notes" ? text : std::string{trim_header_text(text)};
}

Status validate_header_structure(pugi::xml_node rom, std::string_view source)
{
    const auto validate_fields = [source](pugi::xml_node parent, std::span<const char *const> names) -> Status
    {
        for (const auto name : names)
        {
            for (const auto element : parent.children(name))
            {
                if (std::ranges::any_of(element.children(),
                                        [](auto child) { return child.type() == pugi::node_element; }))
                {
                    return invalid(source, std::format("element <{}> child <{}>", parent.name(), name),
                                   "nested elements are not allowed in header text");
                }
            }
        }
        return {};
    };
    if (auto status = validate_fields(rom.child("romid"), kSingletonChildren); !status.has_value())
    {
        return status;
    }
    constexpr std::array kRootFields{"include", "notes"};
    return validate_fields(rom, kRootFields);
}

Result<std::optional<std::uint64_t>> parse_header_address(std::string_view text, std::string_view source,
                                                          std::string_view definition_id)
{
    text = trim_header_text(text);
    if (text.empty())
    {
        return std::optional<std::uint64_t>{};
    }
    if (text.starts_with('+'))
    {
        text.remove_prefix(1);
    }
    const auto parsed = trim_header_text(text) == text ? parse_hex_value(text) : std::nullopt;
    if (!parsed.has_value())
    {
        return invalid(source, "element <romid> child <internalidaddress>",
                       std::format("invalid hexadecimal unsigned value '{}'", text), definition_id);
    }
    return parsed;
}

std::string table_element_text(pugi::xml_node element)
{
    for (const auto child : element.children())
    {
        if (child.type() == pugi::node_cdata)
        {
            return trim_copy(child.value());
        }
        if (child.type() == pugi::node_pcdata)
        {
            auto text = trim_copy(child.value());
            if (!text.empty())
            {
                return text;
            }
        }
    }
    return {};
}

std::string child_text(pugi::xml_node parent, std::string_view child_name)
{
    return table_element_text(parent.child(child_name));
}

Result<pugi::xml_node> identity_element(pugi::xml_node rom, std::string_view source)
{
    if (auto status = validate_header_structure(rom, source); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    const pugi::xml_node rom_id = rom.child("romid");
    if (!rom_id)
    {
        return invalid(source, "element <rom> child <romid>", "missing required identity element");
    }
    if (rom_id.next_sibling("romid"))
    {
        return invalid(source, "element <rom> child <romid>", "duplicate singleton identity element");
    }

    for (const char *child_name : kSingletonChildren)
    {
        const pugi::xml_node child = rom_id.child(child_name);
        if (child && child.next_sibling(child_name))
        {
            return invalid(source, std::format("element <romid> child <{}>", child_name),
                           "duplicate singleton identity element");
        }
    }
    return rom_id;
}

Result<std::string> required_child_text(pugi::xml_node parent, std::string_view parent_name,
                                        std::string_view child_name, std::string_view source)
{
    const std::string value = header_child_text(parent, child_name);
    if (value.empty())
    {
        return invalid(source, std::format("element <{}> child <{}>", parent_name, child_name),
                       "missing or empty required text");
    }
    return value;
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

Result<ParsedRomHeader> parse_rom_header(pugi::xml_node rom, std::string_view source)
{
    auto definition_id = definition_id_for_rom(rom, source);
    if (!definition_id.has_value())
    {
        return std::unexpected(definition_id.error());
    }
    const auto rom_id = rom.child("romid");
    auto address = parse_header_address(read_element_text(rom_id.child("internalidaddress")), source, *definition_id);
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    return ParsedRomHeader{
        .rom = rom,
        .identity = RomIdentity{.xml_id = std::move(*definition_id),
                                .internal_id = header_child_text(rom_id, "internalidstring"),
                                .ecu_id = header_child_text(rom_id, "ecuid"),
                                .internal_id_address = *address},
    };
}

RomMetadata parse_metadata(pugi::xml_node rom_id)
{
    RomMetadata metadata;
    for (const auto& field : kEditableMetadataFields)
    {
        metadata.*field.member = header_child_text(rom_id, field.xml_name);
    }
    metadata.file_size = header_child_text(rom_id, "filesize");
    metadata.notes = header_child_text(rom_id, "notes");
    return metadata;
}

Result<pugi::xml_node> parse_document_root(pugi::xml_document& document, std::span<const std::uint8_t> xml,
                                           std::string_view source, pugi::xml_encoding encoding)
{
    if (const pugi::xml_parse_result parsed =
            document.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata, encoding);
        !parsed)
    {
        return invalid(source, "XML document", std::format("malformed XML: {}", parsed.description()));
    }
    if (std::ranges::count_if(document.children(), [](auto node) { return node.type() == pugi::node_element; }) != 1)
    {
        return invalid(source, "XML document", "expected one document root");
    }
    return document.document_element();
}

Result<pugi::xml_node> parse_root(pugi::xml_document& document, std::span<const std::uint8_t> xml,
                                  std::string_view source, std::string_view root_name)
{
    const auto parsed = parse_document_root(document, xml, source, pugi::encoding_auto);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    const pugi::xml_node root = *parsed;
    if (!root || root.name() != root_name)
    {
        const std::string actual = root ? std::format("<{}>", root.name()) : "no root element";
        return invalid(source, std::format("root element <{}>", root_name),
                       std::format("wrong root; found {}", actual));
    }
    return root;
}

Result<std::uint64_t> parse_hex_unsigned(std::string_view value, std::string_view source, std::string context,
                                         std::string_view definition_id)
{
    if (const std::optional<std::uint64_t> parsed = parse_hex_value(value); parsed.has_value())
    {
        return *parsed;
    }
    return invalid(source, std::move(context), std::format("invalid hexadecimal unsigned value '{}'", trim_copy(value)),
                   definition_id);
}

std::string value_or_empty(pugi::xml_attribute attribute)
{
    return trim_copy(attribute.value());
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

Result<std::optional<std::uint64_t>> optional_hex_attribute(pugi::xml_node node, std::string_view attribute_name,
                                                            std::string_view source, std::string_view definition_id)
{
    const pugi::xml_attribute attribute = node.attribute(attribute_name);
    if (!attribute)
    {
        return std::optional<std::uint64_t>{};
    }

    auto parsed =
        parse_hex_unsigned(attribute.value(), source,
                           std::format("element <{}> attribute '{}'", node.name(), attribute_name), definition_id);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    return std::optional<std::uint64_t>{*parsed};
}

Result<std::optional<std::uint64_t>> optional_address(pugi::xml_node node, std::string_view source,
                                                      std::string_view definition_id)
{
    if (node.attribute("address"))
    {
        return optional_hex_attribute(node, "address", source, definition_id);
    }
    return optional_hex_attribute(node, "storageaddress", source, definition_id);
}

Result<std::uint32_t> dimension_attribute(pugi::xml_node node, std::string_view attribute_name,
                                          std::uint32_t default_value, std::string_view source,
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
        return invalid(source, std::format("element <{}> attribute '{}'", node.name(), attribute_name),
                       std::format("invalid positive dimension '{}'", value), definition_id);
    }
    return static_cast<std::uint32_t>(parsed);
}

Result<std::optional<std::uint32_t>> optional_hex_attribute32(pugi::xml_node node, std::string_view attribute_name,
                                                              std::string_view source, std::string_view definition_id)
{
    auto parsed = optional_hex_attribute(node, attribute_name, source, definition_id);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    if (!parsed->has_value())
    {
        return std::optional<std::uint32_t>{};
    }
    if (**parsed > std::numeric_limits<std::uint32_t>::max())
    {
        return invalid(source, std::format("element <{}> attribute '{}'", node.name(), attribute_name),
                       std::format("hexadecimal value '{}' does not fit in 32 bits", **parsed), definition_id);
    }
    return std::optional<std::uint32_t>{static_cast<std::uint32_t>(**parsed)};
}

Result<std::optional<StorageType>> optional_storage_type_attribute(pugi::xml_node node, std::string_view attribute_name,
                                                                   std::string_view source,
                                                                   std::string_view definition_id)
{
    const pugi::xml_attribute attribute = node.attribute(attribute_name);
    if (!attribute)
    {
        return std::optional<StorageType>{};
    }
    const std::string text = trim_copy(attribute.value());
    if (text.empty())
    {
        return std::optional<StorageType>{};
    }
    auto parsed = storage_type_from_text(text);
    if (!parsed.has_value())
    {
        return invalid(source, std::format("element <{}> attribute '{}'", node.name(), attribute_name),
                       std::format("unrecognized storage type '{}'", text), definition_id);
    }
    return parsed;
}

Result<bool> strict_boolean_attribute(pugi::xml_node node, std::string_view attribute_name, std::string_view source,
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
    return invalid(source, std::format("element <{}> attribute '{}'", node.name(), attribute_name),
                   std::format("invalid strict boolean '{}'; expected 'true' or 'false'", value), definition_id);
}

Status populate_common_axis_attributes(pugi::xml_node table, UnresolvedAxisDefinition& axis, std::string_view source,
                                       std::string_view definition_id)
{
    axis.type = value_or_empty(table.attribute("type"));
    axis.name = value_or_empty(table.attribute("name"));
    auto storage_type = optional_storage_type_attribute(table, "storagetype", source, definition_id);
    if (!storage_type.has_value())
    {
        return std::unexpected(storage_type.error());
    }
    axis.storage_type = *storage_type;
    axis.endian = value_or_empty(table.attribute("endian"));
    if (auto status = populate_optional_hex_dimension(table, "startpos", axis.start_position, source, definition_id);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = populate_optional_hex_dimension(table, "interval", axis.interval, source, definition_id);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (const auto log_parameter = table.attribute("logparam"))
    {
        axis.log_parameter = value_or_empty(log_parameter);
    }
    if (table.child("data"))
    {
        std::vector<std::string> values;
        for (pugi::xml_node data : table.children("data"))
        {
            values.push_back(table_element_text(data));
        }
        axis.static_data = std::move(values);
    }
    return {};
}

void apply_scaling_to_axis(const UnresolvedScaling& scaling, UnresolvedAxisDefinition& axis)
{
    axis.scaling_name = scaling.name;
    axis.units = scaling.units;
    if (scaling.format)
    {
        axis.format = *scaling.format;
    }
    axis.from_byte = scaling.from_byte;
    axis.to_byte = scaling.to_byte;
    if (!axis.storage_type)
    {
        axis.storage_type = scaling.storage_type;
    }
    if (axis.endian.empty())
    {
        axis.endian = scaling.endian;
    }
}

Status populate_common_map_attributes(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                                      std::string_view definition_id)
{
    if (const auto id = table.attribute("id"))
    {
        map.id = value_or_empty(id);
    }
    map.name = value_or_empty(table.attribute("name"));
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
    auto storage_type = optional_storage_type_attribute(table, "storagetype", source, definition_id);
    if (!storage_type.has_value())
    {
        return std::unexpected(storage_type.error());
    }
    map.storage_type = *storage_type;
    map.endian = value_or_empty(table.attribute("endian"));
    if (auto status = populate_optional_hex_dimension(table, "startpos", map.start_position, source, definition_id);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = populate_optional_hex_dimension(table, "interval", map.interval, source, definition_id);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (const auto log_parameter = table.attribute("logparam"))
    {
        map.log_parameter = value_or_empty(log_parameter);
    }
    return {};
}

Status populate_optional_dimension(pugi::xml_node table, std::string_view attribute_name,
                                   std::optional<std::uint32_t>& destination, std::string_view source,
                                   std::string_view definition_id)
{
    if (!table.attribute(attribute_name))
    {
        return {};
    }
    auto dimension = dimension_attribute(table, attribute_name, 1, source, definition_id);
    if (!dimension)
    {
        return std::unexpected(dimension.error());
    }
    destination = *dimension;
    return {};
}

Status populate_optional_hex_dimension(pugi::xml_node table, std::string_view attribute_name,
                                       std::optional<std::uint32_t>& destination, std::string_view source,
                                       std::string_view definition_id)
{
    auto dimension = optional_hex_attribute32(table, attribute_name, source, definition_id);
    if (!dimension.has_value())
    {
        return std::unexpected(dimension.error());
    }
    if (dimension->has_value())
    {
        destination = *dimension;
    }
    return {};
}

Status populate_optional_boolean(pugi::xml_node table, std::string_view attribute_name,
                                 std::optional<bool>& destination, std::string_view source,
                                 std::string_view definition_id)
{
    if (!table.attribute(attribute_name))
    {
        return {};
    }
    auto value = strict_boolean_attribute(table, attribute_name, source, definition_id);
    if (!value)
    {
        return std::unexpected(value.error());
    }
    destination = *value;
    return {};
}

Status populate_map_header(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                           std::string_view definition_id)
{
    if (auto status = populate_common_map_attributes(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (map.name.empty())
    {
        return invalid(source, "element <table> attribute 'name'", "missing or empty map name", definition_id);
    }
    auto address = optional_address(table, source, definition_id);
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    map.address = *address;
    return {};
}

Status populate_map_size(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                         std::string_view definition_id)
{
    if (auto status = populate_optional_dimension(table, "sizex", map.x_size, source, definition_id);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    return populate_optional_dimension(table, "sizey", map.y_size, source, definition_id);
}

Status populate_map_orientation(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                                std::string_view definition_id)
{
    if (auto status = populate_optional_boolean(table, "swapxy", map.swap_xy, source, definition_id);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = populate_optional_boolean(table, "flipx", map.flip_x, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    return populate_optional_boolean(table, "flipy", map.flip_y, source, definition_id);
}

std::string map_scaling_fallback_name(const UnresolvedCalibrationMap& map)
{
    return map.scaling_name.empty() ? map.id.value_or(map.name) : map.scaling_name;
}

void adopt_inline_scaling(const UnresolvedScaling& scaling, UnresolvedCalibrationMap& map)
{
    map.scaling_name = scaling.name;
    if (!map.storage_type)
    {
        map.storage_type = scaling.storage_type;
    }
    if (map.endian.empty())
    {
        map.endian = scaling.endian;
    }
}

} // namespace fastecu::definition
