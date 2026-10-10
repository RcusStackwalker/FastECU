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

std::string TrimCopy(std::string_view value)
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

std::string DetailPrefix(std::string_view source, std::string_view definition_id)
{
    std::string detail = std::format("EcuFlash/RomRaider source '{}'", source);
    if (!definition_id.empty())
    {
        detail += std::format(", definition '{}'", definition_id);
    }
    return detail + ": ";
}

std::unexpected<Error> Invalid(std::string_view source, std::string context, std::string message,
                               std::string_view definition_id)
{
    return Fail(ErrorKind::kInvalidConfig,
                std::format("{}{}: {}", DetailPrefix(source, definition_id), context, message));
}

std::string ReadElementText(pugi::xml_node element)
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

std::string HeaderChildText(pugi::xml_node parent, std::string_view name)
{
    const auto text = ReadElementText(parent.child(name));
    return name == "notes" ? text : std::string{TrimHeaderText(text)};
}

Status ValidateHeaderStructure(pugi::xml_node rom, std::string_view source)
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
                    return Invalid(source, std::format("element <{}> child <{}>", parent.name(), name),
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

namespace
{
// A definition address fits the 32-bit ECU address space; a wider value is a
// malformed definition, not a value to truncate.
Result<memory::DefinitionAddress> DefinitionAddressFrom(std::uint64_t value, std::string_view text,
                                                        std::string_view source, std::string context,
                                                        std::string_view definition_id)
{
    if (value > std::numeric_limits<std::uint32_t>::max())
    {
        return Invalid(source, std::move(context),
                       std::format("invalid hexadecimal unsigned value '{}': does not fit 32 bits", text),
                       definition_id);
    }
    return memory::DefinitionAddress{static_cast<std::uint32_t>(value)};
}
} // namespace

Result<std::optional<memory::DefinitionAddress>> ParseHeaderAddress(std::string_view text, std::string_view source,
                                                                    std::string_view definition_id)
{
    text = TrimHeaderText(text);
    if (text.empty())
    {
        return std::optional<memory::DefinitionAddress>{};
    }
    if (text.starts_with('+'))
    {
        text.remove_prefix(1);
    }
    const auto parsed = TrimHeaderText(text) == text ? ParseHexValue(text) : std::nullopt;
    if (!parsed.has_value())
    {
        return Invalid(source, "element <romid> child <internalidaddress>",
                       std::format("invalid hexadecimal unsigned value '{}'", text), definition_id);
    }
    auto address =
        DefinitionAddressFrom(*parsed, text, source, "element <romid> child <internalidaddress>", definition_id);
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    return std::optional<memory::DefinitionAddress>{*address};
}

std::string TableElementText(pugi::xml_node element)
{
    for (const auto child : element.children())
    {
        if (child.type() == pugi::node_cdata)
        {
            return TrimCopy(child.value());
        }
        if (child.type() == pugi::node_pcdata)
        {
            auto text = TrimCopy(child.value());
            if (!text.empty())
            {
                return text;
            }
        }
    }
    return {};
}

std::string ChildText(pugi::xml_node parent, std::string_view child_name)
{
    return TableElementText(parent.child(child_name));
}

Result<pugi::xml_node> IdentityElement(pugi::xml_node rom, std::string_view source)
{
    if (auto status = ValidateHeaderStructure(rom, source); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    const pugi::xml_node rom_id = rom.child("romid");
    if (!rom_id)
    {
        return Invalid(source, "element <rom> child <romid>", "missing required identity element");
    }
    if (rom_id.next_sibling("romid"))
    {
        return Invalid(source, "element <rom> child <romid>", "duplicate singleton identity element");
    }

    for (const char *child_name : kSingletonChildren)
    {
        const pugi::xml_node child = rom_id.child(child_name);
        if (child && child.next_sibling(child_name))
        {
            return Invalid(source, std::format("element <romid> child <{}>", child_name),
                           "duplicate singleton identity element");
        }
    }
    return rom_id;
}

Result<std::string> RequiredChildText(pugi::xml_node parent, std::string_view parent_name, std::string_view child_name,
                                      std::string_view source)
{
    const std::string value = HeaderChildText(parent, child_name);
    if (value.empty())
    {
        return Invalid(source, std::format("element <{}> child <{}>", parent_name, child_name),
                       "missing or empty required text");
    }
    return value;
}

Result<std::string> DefinitionIdForRom(pugi::xml_node rom, std::string_view source)
{
    auto rom_id = IdentityElement(rom, source);
    if (!rom_id)
    {
        return std::unexpected(rom_id.error());
    }
    return RequiredChildText(*rom_id, "romid", "xmlid", source);
}

Result<ParsedRomHeader> ParseRomHeader(pugi::xml_node rom, std::string_view source)
{
    auto definition_id = DefinitionIdForRom(rom, source);
    if (!definition_id.has_value())
    {
        return std::unexpected(definition_id.error());
    }
    const auto rom_id = rom.child("romid");
    auto address = ParseHeaderAddress(ReadElementText(rom_id.child("internalidaddress")), source, *definition_id);
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    return ParsedRomHeader{
        .rom = rom,
        .identity = RomIdentity{.xml_id = std::move(*definition_id),
                                .internal_id = HeaderChildText(rom_id, "internalidstring"),
                                .ecu_id = HeaderChildText(rom_id, "ecuid"),
                                .internal_id_address = *address},
    };
}

RomMetadata ParseMetadata(pugi::xml_node rom_id)
{
    RomMetadata metadata;
    for (const auto& field : kEditableMetadataFields)
    {
        metadata.*field.member = HeaderChildText(rom_id, field.xml_name);
    }
    metadata.file_size = HeaderChildText(rom_id, "filesize");
    metadata.notes = HeaderChildText(rom_id, "notes");
    return metadata;
}

Result<pugi::xml_node> ParseDocumentRoot(pugi::xml_document& document, std::span<const std::uint8_t> xml,
                                         std::string_view source, pugi::xml_encoding encoding)
{
    if (const pugi::xml_parse_result parsed =
            document.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata, encoding);
        !parsed)
    {
        return Invalid(source, "XML document", std::format("malformed XML: {}", parsed.description()));
    }
    if (std::ranges::count_if(document.children(), [](auto node) { return node.type() == pugi::node_element; }) != 1)
    {
        return Invalid(source, "XML document", "expected one document root");
    }
    return document.document_element();
}

Result<pugi::xml_node> ParseRoot(pugi::xml_document& document, std::span<const std::uint8_t> xml,
                                 std::string_view source, std::string_view root_name)
{
    const auto parsed = ParseDocumentRoot(document, xml, source, pugi::encoding_auto);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    const pugi::xml_node root = *parsed;
    if (!root || root.name() != root_name)
    {
        const std::string actual = root ? std::format("<{}>", root.name()) : "no root element";
        return Invalid(source, std::format("root element <{}>", root_name),
                       std::format("wrong root; found {}", actual));
    }
    return root;
}

Result<std::uint64_t> ParseHexUnsigned(std::string_view value, std::string_view source, std::string context,
                                       std::string_view definition_id)
{
    if (const std::optional<std::uint64_t> parsed = ParseHexValue(value); parsed.has_value())
    {
        return *parsed;
    }
    return Invalid(source, std::move(context), std::format("invalid hexadecimal unsigned value '{}'", TrimCopy(value)),
                   definition_id);
}

std::string ValueOrEmpty(pugi::xml_attribute attribute)
{
    return TrimCopy(attribute.value());
}

std::string SelectionName(std::string name)
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

Result<std::optional<std::uint64_t>> OptionalHexAttribute(pugi::xml_node node, std::string_view attribute_name,
                                                          std::string_view source, std::string_view definition_id)
{
    const pugi::xml_attribute attribute = node.attribute(attribute_name);
    if (!attribute)
    {
        return std::optional<std::uint64_t>{};
    }

    auto parsed =
        ParseHexUnsigned(attribute.value(), source,
                         std::format("element <{}> attribute '{}'", node.name(), attribute_name), definition_id);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    return std::optional<std::uint64_t>{*parsed};
}

Result<std::optional<memory::DefinitionAddress>> OptionalAddress(pugi::xml_node node, std::string_view source,
                                                                 std::string_view definition_id)
{
    const char *name = node.attribute("address") ? "address" : "storageaddress";
    auto value = OptionalHexAttribute(node, name, source, definition_id);
    if (!value.has_value())
    {
        return std::unexpected(value.error());
    }
    if (!value->has_value())
    {
        return std::optional<memory::DefinitionAddress>{};
    }
    auto address = DefinitionAddressFrom(**value, TrimCopy(node.attribute(name).value()), source,
                                         std::format("element <{}> attribute '{}'", node.name(), name), definition_id);
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    return std::optional<memory::DefinitionAddress>{*address};
}

Result<std::uint32_t> DimensionAttribute(pugi::xml_node node, std::string_view attribute_name,
                                         std::uint32_t default_value, std::string_view source,
                                         std::string_view definition_id)
{
    const pugi::xml_attribute attribute = node.attribute(attribute_name);
    if (!attribute)
    {
        return default_value;
    }

    const std::string value = TrimCopy(attribute.value());
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max())
    {
        return Invalid(source, std::format("element <{}> attribute '{}'", node.name(), attribute_name),
                       std::format("invalid positive dimension '{}'", value), definition_id);
    }
    return static_cast<std::uint32_t>(parsed);
}

Result<std::optional<std::uint32_t>> OptionalHexAttribute32(pugi::xml_node node, std::string_view attribute_name,
                                                            std::string_view source, std::string_view definition_id)
{
    auto parsed = OptionalHexAttribute(node, attribute_name, source, definition_id);
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
        return Invalid(source, std::format("element <{}> attribute '{}'", node.name(), attribute_name),
                       std::format("hexadecimal value '{}' does not fit in 32 bits", **parsed), definition_id);
    }
    return std::optional<std::uint32_t>{static_cast<std::uint32_t>(**parsed)};
}

Result<std::optional<StorageType>> OptionalStorageTypeAttribute(pugi::xml_node node, std::string_view attribute_name,
                                                                std::string_view source, std::string_view definition_id)
{
    const pugi::xml_attribute attribute = node.attribute(attribute_name);
    if (!attribute)
    {
        return std::optional<StorageType>{};
    }
    const std::string text = TrimCopy(attribute.value());
    if (text.empty())
    {
        return std::optional<StorageType>{};
    }
    auto parsed = StorageTypeFromText(text);
    if (!parsed.has_value())
    {
        return Invalid(source, std::format("element <{}> attribute '{}'", node.name(), attribute_name),
                       std::format("unrecognized storage type '{}'", text), definition_id);
    }
    return parsed;
}

Result<bool> StrictBooleanAttribute(pugi::xml_node node, std::string_view attribute_name, std::string_view source,
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
    return Invalid(source, std::format("element <{}> attribute '{}'", node.name(), attribute_name),
                   std::format("invalid strict boolean '{}'; expected 'true' or 'false'", value), definition_id);
}

Status PopulateCommonAxisAttributes(pugi::xml_node table, UnresolvedAxisDefinition& axis, std::string_view source,
                                    std::string_view definition_id)
{
    axis.type = ValueOrEmpty(table.attribute("type"));
    axis.name = ValueOrEmpty(table.attribute("name"));
    auto storage_type = OptionalStorageTypeAttribute(table, "storagetype", source, definition_id);
    if (!storage_type.has_value())
    {
        return std::unexpected(storage_type.error());
    }
    axis.storage_type = *storage_type;
    axis.endian = ValueOrEmpty(table.attribute("endian"));
    if (auto status = PopulateOptionalHexDimension(table, "startpos", axis.start_position, source, definition_id);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = PopulateOptionalHexDimension(table, "interval", axis.interval, source, definition_id);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (const auto log_parameter = table.attribute("logparam"))
    {
        axis.log_parameter = ValueOrEmpty(log_parameter);
    }
    if (table.child("data"))
    {
        std::vector<std::string> values;
        for (pugi::xml_node data : table.children("data"))
        {
            values.push_back(TableElementText(data));
        }
        axis.static_data = std::move(values);
    }
    return {};
}

void ApplyScalingToAxis(const UnresolvedScaling& scaling, UnresolvedAxisDefinition& axis)
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

Status PopulateCommonMapAttributes(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                                   std::string_view definition_id)
{
    if (const auto id = table.attribute("id"))
    {
        map.id = ValueOrEmpty(id);
    }
    map.name = ValueOrEmpty(table.attribute("name"));
    map.type = ValueOrEmpty(table.attribute("type"));
    map.category = ValueOrEmpty(table.attribute("category"));
    map.subcategory = ValueOrEmpty(table.attribute("subcategory"));
    map.description = ValueOrEmpty(table.attribute("description"));
    if (map.description.empty())
    {
        map.description = ChildText(table, "description");
    }
    map.level = ValueOrEmpty(table.attribute("level"));
    map.user_level = ValueOrEmpty(table.attribute("userlevel"));
    auto storage_type = OptionalStorageTypeAttribute(table, "storagetype", source, definition_id);
    if (!storage_type.has_value())
    {
        return std::unexpected(storage_type.error());
    }
    map.storage_type = *storage_type;
    map.endian = ValueOrEmpty(table.attribute("endian"));
    if (auto status = PopulateOptionalHexDimension(table, "startpos", map.start_position, source, definition_id);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = PopulateOptionalHexDimension(table, "interval", map.interval, source, definition_id);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (const auto log_parameter = table.attribute("logparam"))
    {
        map.log_parameter = ValueOrEmpty(log_parameter);
    }
    return {};
}

Status PopulateOptionalDimension(pugi::xml_node table, std::string_view attribute_name,
                                 std::optional<std::uint32_t>& destination, std::string_view source,
                                 std::string_view definition_id)
{
    if (!table.attribute(attribute_name))
    {
        return {};
    }
    auto dimension = DimensionAttribute(table, attribute_name, 1, source, definition_id);
    if (!dimension)
    {
        return std::unexpected(dimension.error());
    }
    destination = *dimension;
    return {};
}

Status PopulateOptionalHexDimension(pugi::xml_node table, std::string_view attribute_name,
                                    std::optional<std::uint32_t>& destination, std::string_view source,
                                    std::string_view definition_id)
{
    auto dimension = OptionalHexAttribute32(table, attribute_name, source, definition_id);
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

Status PopulateOptionalBoolean(pugi::xml_node table, std::string_view attribute_name, std::optional<bool>& destination,
                               std::string_view source, std::string_view definition_id)
{
    if (!table.attribute(attribute_name))
    {
        return {};
    }
    auto value = StrictBooleanAttribute(table, attribute_name, source, definition_id);
    if (!value)
    {
        return std::unexpected(value.error());
    }
    destination = *value;
    return {};
}

Status PopulateMapHeader(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                         std::string_view definition_id)
{
    if (auto status = PopulateCommonMapAttributes(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (map.name.empty())
    {
        return Invalid(source, "element <table> attribute 'name'", "missing or empty map name", definition_id);
    }
    auto address = OptionalAddress(table, source, definition_id);
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    map.address = *address;
    return {};
}

Status PopulateMapSize(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                       std::string_view definition_id)
{
    if (auto status = PopulateOptionalDimension(table, "sizex", map.x_size, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    return PopulateOptionalDimension(table, "sizey", map.y_size, source, definition_id);
}

Status PopulateMapOrientation(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                              std::string_view definition_id)
{
    if (auto status = PopulateOptionalBoolean(table, "swapxy", map.swap_xy, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = PopulateOptionalBoolean(table, "flipx", map.flip_x, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    return PopulateOptionalBoolean(table, "flipy", map.flip_y, source, definition_id);
}

std::string MapScalingFallbackName(const UnresolvedCalibrationMap& map)
{
    return map.scaling_name.empty() ? map.id.value_or(map.name) : map.scaling_name;
}

void AdoptInlineScaling(const UnresolvedScaling& scaling, UnresolvedCalibrationMap& map)
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
