#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"

namespace fastecu::definition
{

// Returned nodes borrow the caller-owned document. Root/format policy remains with the caller.
Result<pugi::xml_node> ParseDocumentRoot(pugi::xml_document& document, std::span<const std::uint8_t> xml,
                                         std::string_view source, pugi::xml_encoding encoding);
// Concatenate direct text/CDATA children; never traverse nested elements.
std::string ReadElementText(pugi::xml_node element);
std::string HeaderChildText(pugi::xml_node parent, std::string_view name);
Status ValidateHeaderStructure(pugi::xml_node rom, std::string_view source);
Result<std::optional<std::uint64_t>> ParseHeaderAddress(std::string_view text, std::string_view source,
                                                        std::string_view definition_id = {});

struct ParsedRomHeader
{
    pugi::xml_node rom;   // Borrowed ROM node; its romid child has been validated.
    RomIdentity identity; // Owns its strings.
};
Result<ParsedRomHeader> ParseRomHeader(pugi::xml_node rom, std::string_view source);

std::string TrimCopy(std::string_view value);
std::string DetailPrefix(std::string_view source, std::string_view definition_id = {});
std::unexpected<Error> Invalid(std::string_view source, std::string context, std::string message,
                               std::string_view definition_id = {});
// First table text value, skipping whitespace-only PCDATA introduced by header whitespace preservation.
std::string TableElementText(pugi::xml_node element);
std::string ChildText(pugi::xml_node parent, std::string_view child_name);
Result<pugi::xml_node> IdentityElement(pugi::xml_node rom, std::string_view source);
Result<std::string> RequiredChildText(pugi::xml_node parent, std::string_view parent_name, std::string_view child_name,
                                      std::string_view source);
Result<std::string> DefinitionIdForRom(pugi::xml_node rom, std::string_view source);
RomMetadata ParseMetadata(pugi::xml_node rom_id);
Result<pugi::xml_node> ParseRoot(pugi::xml_document& document, std::span<const std::uint8_t> xml,
                                 std::string_view source, std::string_view root_name);
Result<std::uint64_t> ParseHexUnsigned(std::string_view value, std::string_view source, std::string context,
                                       std::string_view definition_id);
std::string ValueOrEmpty(pugi::xml_attribute attribute);
std::string SelectionName(std::string name);
Result<std::optional<std::uint64_t>> OptionalHexAttribute(pugi::xml_node node, std::string_view attribute_name,
                                                          std::string_view source, std::string_view definition_id);
Result<std::optional<std::uint64_t>> OptionalAddress(pugi::xml_node node, std::string_view source,
                                                     std::string_view definition_id);
Result<std::uint32_t> DimensionAttribute(pugi::xml_node node, std::string_view attribute_name,
                                         std::uint32_t default_value, std::string_view source,
                                         std::string_view definition_id);
Result<bool> StrictBooleanAttribute(pugi::xml_node node, std::string_view attribute_name, std::string_view source,
                                    std::string_view definition_id);
Result<std::optional<std::uint32_t>> OptionalHexAttribute32(pugi::xml_node node, std::string_view attribute_name,
                                                            std::string_view source, std::string_view definition_id);
Status PopulateOptionalHexDimension(pugi::xml_node table, std::string_view attribute_name,
                                    std::optional<std::uint32_t>& destination, std::string_view source,
                                    std::string_view definition_id);
Result<std::optional<StorageType>> OptionalStorageTypeAttribute(pugi::xml_node node, std::string_view attribute_name,
                                                                std::string_view source,
                                                                std::string_view definition_id);
Status PopulateCommonAxisAttributes(pugi::xml_node table, UnresolvedAxisDefinition& axis, std::string_view source,
                                    std::string_view definition_id);
void ApplyScalingToAxis(const UnresolvedScaling& scaling, UnresolvedAxisDefinition& axis);
Status PopulateCommonMapAttributes(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                                   std::string_view definition_id);
Status PopulateOptionalDimension(pugi::xml_node table, std::string_view attribute_name,
                                 std::optional<std::uint32_t>& destination, std::string_view source,
                                 std::string_view definition_id);
Status PopulateOptionalBoolean(pugi::xml_node table, std::string_view attribute_name, std::optional<bool>& destination,
                               std::string_view source, std::string_view definition_id);

// The <table> attributes and checks both definition formats read the same way. Each parser
// keeps its own parse_table / parse_axis and composes these with its format-specific rules.
Status PopulateMapHeader(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                         std::string_view definition_id);
Status PopulateMapSize(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                       std::string_view definition_id);
Status PopulateMapOrientation(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                              std::string_view definition_id);
std::string MapScalingFallbackName(const UnresolvedCalibrationMap& map);
void AdoptInlineScaling(const UnresolvedScaling& scaling, UnresolvedCalibrationMap& map);

// Reads the map's scaling reference and parses its inline <scaling> child, if any, into the map.
// The scaling is returned rather than stored so the caller can apply format rules before keeping it.
// ParseScaling: Result<UnresolvedScaling>(pugi::xml_node scaling_node, std::string fallback_name).
template <typename ParseScaling>
Result<std::optional<UnresolvedScaling>> ParseMapScaling(pugi::xml_node table, UnresolvedCalibrationMap& map,
                                                         ParseScaling parse_scaling)
{
    map.scaling_name = ValueOrEmpty(table.attribute("scaling"));
    const pugi::xml_node scaling_node = table.child("scaling");
    if (!scaling_node)
    {
        return std::optional<UnresolvedScaling>{};
    }
    auto scaling = parse_scaling(scaling_node, MapScalingFallbackName(map));
    if (!scaling.has_value())
    {
        return std::unexpected(scaling.error());
    }
    AdoptInlineScaling(*scaling, map);
    return std::optional<UnresolvedScaling>{std::move(*scaling)};
}

// ParseScaling: as for parse_map_scaling.
template <typename ParseScaling>
Result<UnresolvedAxisDefinition> ParseAxisDefinition(pugi::xml_node table, std::uint32_t default_size,
                                                     std::string_view source, std::string_view definition_id,
                                                     std::vector<UnresolvedScaling>& scalings,
                                                     ParseScaling parse_scaling)
{
    UnresolvedAxisDefinition axis;
    if (auto status = PopulateCommonAxisAttributes(table, axis, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (axis.name.empty())
    {
        return Invalid(source, std::format("element <table> type '{}' attribute 'name'", axis.type),
                       "missing or empty axis name", definition_id);
    }
    auto address = OptionalAddress(table, source, definition_id);
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    axis.address = *address;

    if (const char *size_attribute =
            table.attribute("elements") ? "elements" : (table.attribute("size") ? "size" : nullptr);
        size_attribute)
    {
        auto size = DimensionAttribute(table, size_attribute, default_size, source, definition_id);
        if (!size.has_value())
        {
            return std::unexpected(size.error());
        }
        axis.size = *size;
    }
    axis.scaling_name = ValueOrEmpty(table.attribute("scaling"));
    if (const pugi::xml_node scaling_node = table.child("scaling"))
    {
        auto scaling = parse_scaling(scaling_node, axis.scaling_name.empty() ? axis.name : axis.scaling_name);
        if (!scaling.has_value())
        {
            return std::unexpected(scaling.error());
        }
        ApplyScalingToAxis(*scaling, axis);
        scalings.push_back(std::move(*scaling));
    }
    return axis;
}

template <typename ParseAxis>
Status PopulateAxes(pugi::xml_node table, UnresolvedCalibrationMap& map, std::string_view source,
                    std::string_view definition_id, std::vector<UnresolvedScaling>& scalings, ParseAxis parse_axis)
{
    bool x_axis_populated = false;
    bool y_axis_populated = false;
    for (pugi::xml_node axis_table : table.children("table"))
    {
        const std::string type = ValueOrEmpty(axis_table.attribute("type"));
        if (type == "X Axis" || type == "Static X Axis" || type == "Static Y Axis" ||
            (type == "Y Axis" && map.type == "2D"))
        {
            if (x_axis_populated)
            {
                return Invalid(source, std::format("element <table> child <table> type '{}'", type),
                               "second axis targets already-populated X axis slot", definition_id);
            }
            x_axis_populated = true;
            if (type == "Static Y Axis" || type == "Y Axis")
            {
                map.x_size = map.y_size;
                map.y_size = 1;
            }
            auto axis = parse_axis(axis_table, map.x_size.value_or(1U), source, definition_id, scalings);
            if (!axis)
            {
                return std::unexpected(axis.error());
            }
            if (axis->type == "Static Y Axis")
            {
                axis->type = "Static X Axis";
            }
            if ((type == "Static Y Axis" || type == "Y Axis") && !axis->size)
            {
                axis->size = map.x_size.value_or(1U);
            }
            map.x_axis = std::move(*axis);
        }
        else if (type == "Y Axis")
        {
            if (y_axis_populated)
            {
                return Invalid(source, std::format("element <table> child <table> type '{}'", type),
                               "second axis targets already-populated Y axis slot", definition_id);
            }
            y_axis_populated = true;
            auto axis = parse_axis(axis_table, map.y_size.value_or(1U), source, definition_id, scalings);
            if (!axis)
            {
                return std::unexpected(axis.error());
            }
            map.y_axis = std::move(*axis);
        }
    }
    return {};
}

} // namespace fastecu::definition
