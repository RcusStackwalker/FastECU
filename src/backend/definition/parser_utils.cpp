#include "src/backend/definition/parser_utils.h"

#include <array>
#include <charconv>
#include <cctype>
#include <format>
#include <limits>
#include <utility>

namespace fastecu::definition
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

std::string detail_prefix(std::string_view source, std::string_view definition_id)
{
    std::string detail = std::format("EcuFlash/RomRaider source '{}'", source);
    if (!definition_id.empty())
    {
        detail += std::format(", definition '{}'", definition_id);
    }
    return detail + ": ";
}

std::unexpected<Error> invalid(
    std::string_view source,
    std::string context,
    std::string message,
    std::string_view definition_id)
{
    return fail(
        ErrorKind::InvalidConfig,
        std::format("{}{}: {}", detail_prefix(source, definition_id), context, message));
}

std::string child_text(pugi::xml_node parent, std::string_view child_name)
{
    const pugi::xml_node child = parent.child(child_name);
    return child ? trim_copy(child.child_value()) : std::string{};
}

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
                std::format("element <romid> child <{}>", child_name),
                "duplicate singleton identity element");
        }
    }
    return rom_id;
}

Result<std::string> required_child_text(
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

Result<std::string> definition_id_for_rom(
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

Result<pugi::xml_node> parse_root(
    pugi::xml_document& document,
    std::span<const std::uint8_t> xml,
    std::string_view source,
    std::string_view root_name)
{
    const pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size());
    if (!parsed)
    {
        return invalid(source, "XML document", std::format("malformed XML: {}", parsed.description()));
    }

    const pugi::xml_node root = document.document_element();
    if (!root || root.name() != root_name)
    {
        const std::string actual = root ? std::format("<{}>", root.name()) : "no root element";
        return invalid(source, std::format("root element <{}>", root_name), std::format("wrong root; found {}", actual));
    }
    return root;
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

Result<std::optional<std::uint64_t>> optional_hex_element(
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
    if (!parsed)
    {
        return std::unexpected(parsed.error());
    }
    return std::optional<std::uint64_t>{*parsed};
}

Result<std::optional<std::uint64_t>> optional_hex_attribute(
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

Result<std::optional<std::uint32_t>> optional_hex_attribute32(
    pugi::xml_node node,
    std::string_view attribute_name,
    std::string_view source,
    std::string_view definition_id)
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
        return invalid(
            source,
            std::format("element <{}> attribute '{}'", node.name(), attribute_name),
            std::format("hexadecimal value '{}' does not fit in 32 bits", **parsed),
            definition_id);
    }
    return std::optional<std::uint32_t>{static_cast<std::uint32_t>(**parsed)};
}

Result<std::optional<StorageType>> optional_storage_type_attribute(
    pugi::xml_node node,
    std::string_view attribute_name,
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
        return invalid(
            source,
            std::format("element <{}> attribute '{}'", node.name(), attribute_name),
            std::format("unrecognized storage type '{}'", text),
            definition_id);
    }
    return parsed;
}

Result<bool> strict_boolean_attribute(
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

Status populate_common_axis_attributes(
    pugi::xml_node table,
    UnresolvedAxisDefinition& axis,
    std::string_view source,
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
            values.push_back(trim_copy(data.child_value()));
        }
        axis.static_data = std::move(values);
    }
    return {};
}

void apply_scaling_to_axis(
    const UnresolvedScaling& scaling,
    UnresolvedAxisDefinition& axis)
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

Status populate_common_map_attributes(
    pugi::xml_node table,
    UnresolvedCalibrationMap& map,
    std::string_view source,
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

Status populate_optional_dimension(
    pugi::xml_node table,
    std::string_view attribute_name,
    std::optional<std::uint32_t>& destination,
    std::string_view source,
    std::string_view definition_id)
{
    if (!table.attribute(attribute_name))
    {
        return {};
    }
    auto dimension =
        dimension_attribute(table, attribute_name, 1, source, definition_id);
    if (!dimension)
    {
        return std::unexpected(dimension.error());
    }
    destination = *dimension;
    return {};
}

Status populate_optional_hex_dimension(
    pugi::xml_node table,
    std::string_view attribute_name,
    std::optional<std::uint32_t>& destination,
    std::string_view source,
    std::string_view definition_id)
{
    auto dimension = optional_hex_attribute32(table, attribute_name, source, definition_id);
    if (!dimension.has_value())
    {
        return std::unexpected(dimension.error());
    }
    if (dimension->has_value())
    {
        destination = **dimension;
    }
    return {};
}

Status populate_optional_boolean(
    pugi::xml_node table,
    std::string_view attribute_name,
    std::optional<bool>& destination,
    std::string_view source,
    std::string_view definition_id)
{
    if (!table.attribute(attribute_name))
    {
        return {};
    }
    auto value =
        strict_boolean_attribute(table, attribute_name, source, definition_id);
    if (!value)
    {
        return std::unexpected(value.error());
    }
    destination = *value;
    return {};
}

} // namespace fastecu::definition
