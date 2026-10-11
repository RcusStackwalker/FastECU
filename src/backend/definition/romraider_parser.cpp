#include "src/backend/definition/romraider_parser.h"
#include "src/backend/definition/parser_utils.h"
#include "src/backend/definition/text_format.h"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <pugixml.hpp>

#include "src/backend/ports/error.h"

using namespace std::literals::string_view_literals;

namespace fastecu::definition
{
namespace
{

void AppendSelections(pugi::xml_node parent, UnresolvedScaling& scaling)
{
    for (pugi::xml_node state : parent.children("state"))
    {
        scaling.selections.emplace_back(SelectionName(ValueOrEmpty(state.attribute("name"))),
                                        ValueOrEmpty(state.attribute("data")));
    }
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

Result<UnresolvedScaling> ParseScaling(pugi::xml_node scaling_node, std::string fallback_name, pugi::xml_node owner,
                                       std::string_view source, std::string_view definition_id)
{
    UnresolvedScaling scaling;
    scaling.name = ValueOrEmpty(scaling_node.attribute("name"));
    if (scaling.name.empty())
    {
        scaling.name = std::move(fallback_name);
    }
    scaling.units = ValueOrEmpty(scaling_node.attribute("units"));
    if (const auto expression = scaling_node.attribute("expression"))
    {
        scaling.from_byte = ValueOrEmpty(expression);
    }
    if (const auto to_byte = scaling_node.attribute("to_byte"))
    {
        scaling.to_byte = ValueOrEmpty(to_byte);
    }
    if (const auto format = scaling_node.attribute("format"))
    {
        scaling.format = ValueOrEmpty(format);
    }
    scaling.minimum = ValueOrEmpty(scaling_node.attribute("minimum"));
    if (scaling.minimum.empty())
    {
        scaling.minimum = ValueOrEmpty(scaling_node.attribute("min"));
    }
    if (scaling.minimum.empty())
    {
        scaling.minimum = ValueOrEmpty(owner.attribute("minvalue"));
    }
    scaling.maximum = ValueOrEmpty(scaling_node.attribute("maximum"));
    if (scaling.maximum.empty())
    {
        scaling.maximum = ValueOrEmpty(scaling_node.attribute("max"));
    }
    if (scaling.maximum.empty())
    {
        scaling.maximum = ValueOrEmpty(owner.attribute("maxvalue"));
    }
    scaling.coarse_increment = ValueOrEmpty(scaling_node.attribute("coarseincrement"));
    scaling.fine_increment = ValueOrEmpty(scaling_node.attribute("fineincrement"));
    auto storage_type = OptionalStorageTypeAttribute(scaling_node, "storagetype", source, definition_id);
    if (!storage_type.has_value())
    {
        return std::unexpected(storage_type.error());
    }
    scaling.storage_type = *storage_type;
    if (!scaling.storage_type)
    {
        auto owner_storage_type = OptionalStorageTypeAttribute(owner, "storagetype", source, definition_id);
        if (!owner_storage_type.has_value())
        {
            return std::unexpected(owner_storage_type.error());
        }
        scaling.storage_type = *owner_storage_type;
    }
    scaling.endian = ValueOrEmpty(scaling_node.attribute("endian"));
    if (scaling.endian.empty())
    {
        scaling.endian = ValueOrEmpty(owner.attribute("endian"));
    }
    AppendSelections(scaling_node, scaling);
    return scaling;
}

Result<UnresolvedAxisDefinition> ParseAxis(pugi::xml_node table, std::uint32_t default_size, std::string_view source,
                                           std::string_view definition_id, std::vector<UnresolvedScaling>& scalings)
{
    return ParseAxisDefinition(
        table, default_size, source, definition_id, scalings,
        [&](pugi::xml_node scaling_node, std::string fallback_name)
        { return ParseScaling(scaling_node, std::move(fallback_name), table, source, definition_id); });
}

Result<UnresolvedCalibrationMap> ParseTable(pugi::xml_node table, std::string_view source,
                                            std::string_view definition_id, std::vector<UnresolvedScaling>& scalings)
{
    UnresolvedCalibrationMap map;
    if (auto status = PopulateMapHeader(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = PopulateMapSize(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = PopulateMapOrientation(table, map, source, definition_id); !status.has_value())
    {
        return std::unexpected(status.error());
    }

    auto parsed_scaling =
        ParseMapScaling(table, map, [&](pugi::xml_node scaling_node, std::string fallback_name)
                        { return ParseScaling(scaling_node, std::move(fallback_name), table, source, definition_id); });
    if (!parsed_scaling.has_value())
    {
        return std::unexpected(parsed_scaling.error());
    }
    std::optional<UnresolvedScaling>& scaling = *parsed_scaling;
    if (scaling.has_value())
    {
        AppendSelections(table, *scaling);
    }

    // A Switch is a selectable map whose <state> children are its selections.
    if (map.type == "Switch")
    {
        map.type = "Selectable";
        map.storage_type = StorageType::kBloblist;
        if (!scaling.has_value())
        {
            scaling.emplace();
            scaling->name = MapScalingFallbackName(map);
            AppendSelections(table, *scaling);
            map.scaling_name = scaling->name;
        }
        scaling->storage_type = StorageType::kBloblist;
    }
    if (scaling.has_value())
    {
        scalings.push_back(std::move(*scaling));
    }

    if (auto status = PopulateAxes(table, map, source, definition_id, scalings, ParseAxis); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    return map;
}

std::vector<std::string> ParentReferences(pugi::xml_node rom)
{
    const std::string parent{TrimHeaderText(rom.attribute("base").value())};
    return parent.empty() ? std::vector<std::string>{} : std::vector<std::string>{parent};
}

} // namespace

Result<std::vector<DefinitionIndexEntry>> ParseRomraiderIndex(std::span<const std::uint8_t> xml,
                                                              std::string_view source)
{
    pugi::xml_document document;
    auto root = ParseRoot(document, xml, source, "roms"sv);
    if (!root.has_value())
    {
        return std::unexpected(root.error());
    }

    std::vector<DefinitionIndexEntry> entries;
    for (pugi::xml_node rom : root->children("rom"))
    {
        auto header = ParseRomHeader(rom, source);
        if (!header.has_value())
        {
            return std::unexpected(header.error());
        }

        entries.push_back(DefinitionIndexEntry{
            .format = DefinitionFormat::kRomRaider,
            .definition_id = std::move(header->identity.xml_id),
            .internal_id = std::move(header->identity.internal_id),
            .internal_id_address = header->identity.internal_id_address,
            .internal_id_encoding = IdEncoding::kAsciiOrHex,
            .ecu_id = std::move(header->identity.ecu_id),
            .flash_method = HeaderChildText(rom.child("romid"), "flashmethod"),
            .source = std::string{source},
            .parents = ParentReferences(rom),
        });
    }
    return entries;
}

Result<UnresolvedDefinition> ParseRomraiderDefinition(std::span<const std::uint8_t> xml, std::string_view source,
                                                      std::string_view definition_id)
{
    if (definition_id.empty())
    {
        return Invalid(source, "definition ID", "missing or empty required definition identity");
    }

    pugi::xml_document document;
    auto root = ParseRoot(document, xml, source, "roms"sv);
    if (!root.has_value())
    {
        return std::unexpected(root.error());
    }

    pugi::xml_node selected_rom;
    for (pugi::xml_node rom : root->children("rom"))
    {
        auto candidate_id = DefinitionIdForRom(rom, source);
        if (!candidate_id)
        {
            return std::unexpected(candidate_id.error());
        }
        if (*candidate_id == definition_id)
        {
            if (selected_rom)
            {
                return Invalid(source, "element <romid> child <xmlid>", "duplicate definition identity", definition_id);
            }
            selected_rom = rom;
        }
    }

    if (!selected_rom)
    {
        return Invalid(source, "element <romid> child <xmlid>", "definition ID not found", definition_id);
    }

    auto header = ParseRomHeader(selected_rom, source);
    if (!header.has_value())
    {
        return std::unexpected(header.error());
    }

    UnresolvedDefinition definition{
        .format = DefinitionFormat::kRomRaider,
        .source = std::string{source},
        .identity = std::move(header->identity),
        .metadata = ParseMetadata(header->rom.child("romid")),
        .parents = ParentReferences(selected_rom),
    };

    std::unordered_set<std::string> map_ids;
    for (pugi::xml_node table : selected_rom.children("table"))
    {
        auto map = ParseTable(table, source, definition_id, definition.scalings);
        if (!map)
        {
            return std::unexpected(map.error());
        }
        if (const std::string map_id = map->id.value_or(map->name); !map_ids.insert(map_id).second)
        {
            return Invalid(source, "element <table> attribute 'id' or 'name'",
                           std::format("duplicate map identity '{}'", map_id), definition_id);
        }
        definition.maps.push_back(std::move(*map));
    }
    return definition;
}

} // namespace fastecu::definition
