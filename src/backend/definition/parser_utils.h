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

std::string trim_copy(std::string_view value);
std::string detail_prefix(std::string_view source, std::string_view definition_id = {});
std::unexpected<Error> invalid(
    std::string_view source,
    std::string context,
    std::string message,
    std::string_view definition_id = {});
std::string child_text(pugi::xml_node parent, std::string_view child_name);
Result<pugi::xml_node> identity_element(
    pugi::xml_node rom,
    std::string_view source);
Result<std::string> required_child_text(
    pugi::xml_node parent,
    std::string_view parent_name,
    std::string_view child_name,
    std::string_view source);
Result<std::string> definition_id_for_rom(
    pugi::xml_node rom,
    std::string_view source);
RomMetadata parse_metadata(pugi::xml_node rom_id);
Result<pugi::xml_node> parse_root(
    pugi::xml_document& document,
    std::span<const std::uint8_t> xml,
    std::string_view source,
    std::string_view root_name);
Result<std::uint64_t> parse_hex_unsigned(
    std::string_view value,
    std::string_view source,
    std::string context,
    std::string_view definition_id);
std::string value_or_empty(pugi::xml_attribute attribute);
std::string selection_name(std::string name);
Result<std::optional<std::uint64_t>> optional_hex_element(
    pugi::xml_node parent,
    std::string_view child_name,
    std::string_view source,
    std::string_view definition_id);
Result<std::optional<std::uint64_t>> optional_hex_attribute(
    pugi::xml_node node,
    std::string_view attribute_name,
    std::string_view source,
    std::string_view definition_id);
Result<std::optional<std::uint64_t>> optional_address(
    pugi::xml_node node,
    std::string_view source,
    std::string_view definition_id);
Result<std::uint32_t> dimension_attribute(
    pugi::xml_node node,
    std::string_view attribute_name,
    std::uint32_t default_value,
    std::string_view source,
    std::string_view definition_id);
Result<bool> strict_boolean_attribute(
    pugi::xml_node node,
    std::string_view attribute_name,
    std::string_view source,
    std::string_view definition_id);
Result<std::optional<std::uint32_t>> optional_hex_attribute32(
    pugi::xml_node node,
    std::string_view attribute_name,
    std::string_view source,
    std::string_view definition_id);
Status populate_optional_hex_dimension(
    pugi::xml_node table,
    std::string_view attribute_name,
    std::optional<std::uint32_t>& destination,
    std::string_view source,
    std::string_view definition_id);
Result<std::optional<StorageType>> optional_storage_type_attribute(
    pugi::xml_node node,
    std::string_view attribute_name,
    std::string_view source,
    std::string_view definition_id);
Status populate_common_axis_attributes(
    pugi::xml_node table,
    UnresolvedAxisDefinition& axis,
    std::string_view source,
    std::string_view definition_id);
void apply_scaling_to_axis(
    const UnresolvedScaling& scaling,
    UnresolvedAxisDefinition& axis);
Status populate_common_map_attributes(
    pugi::xml_node table,
    UnresolvedCalibrationMap& map,
    std::string_view source,
    std::string_view definition_id);
Status populate_optional_dimension(
    pugi::xml_node table,
    std::string_view attribute_name,
    std::optional<std::uint32_t>& destination,
    std::string_view source,
    std::string_view definition_id);
Status populate_optional_boolean(
    pugi::xml_node table,
    std::string_view attribute_name,
    std::optional<bool>& destination,
    std::string_view source,
    std::string_view definition_id);

template <typename ParseAxis>
Status populate_axes(
    pugi::xml_node table,
    UnresolvedCalibrationMap& map,
    std::string_view source,
    std::string_view definition_id,
    std::vector<UnresolvedScaling>& scalings,
    ParseAxis parse_axis)
{
    bool x_axis_populated = false;
    bool y_axis_populated = false;
    for (pugi::xml_node axis_table : table.children("table"))
    {
        const std::string type = value_or_empty(axis_table.attribute("type"));
        if (type == "X Axis" || type == "Static X Axis" || type == "Static Y Axis" ||
            (type == "Y Axis" && map.type == "2D"))
        {
            if (x_axis_populated)
            {
                return invalid(
                    source,
                    std::format("element <table> child <table> type '{}'", type),
                    "second axis targets already-populated X axis slot",
                    definition_id);
            }
            x_axis_populated = true;
            if (type == "Static Y Axis" || type == "Y Axis")
            {
                map.x_size = map.y_size;
                map.y_size = 1;
            }
            auto axis = parse_axis(
                axis_table, map.x_size.value_or(1U), source, definition_id, scalings);
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
                return invalid(
                    source,
                    std::format("element <table> child <table> type '{}'", type),
                    "second axis targets already-populated Y axis slot",
                    definition_id);
            }
            y_axis_populated = true;
            auto axis = parse_axis(
                axis_table, map.y_size.value_or(1U), source, definition_id, scalings);
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
