#include "src/backend/config/car_model_catalog.h"

#include <algorithm>
#include <format>
#include <string_view>

#include <pugixml.hpp>

namespace fastecu::config
{
namespace
{

std::string text_or_empty(pugi::xml_node car_model, std::string_view tag)
{
    return car_model.child(tag).text().as_string();
}

} // namespace

Result<CarModelCatalog> load_car_model_catalog(const ConfigPaths& paths, IFileRepository& file_repository)
{
    Result<std::vector<std::uint8_t>> bytes = file_repository.read(paths.protocols_file);
    if (!bytes.has_value())
    {
        return std::unexpected(bytes.error());
    }

    pugi::xml_document doc;
    if (pugi::xml_parse_result parsed = doc.load_buffer(bytes->data(), bytes->size()); !parsed)
    {
        return fail(ErrorKind::InvalidConfig, std::format("protocols parse error: {}", parsed.description()));
    }

    CarModelCatalog catalog;
    // <car_models> is a sibling of <protocols> under <config>, not nested
    // inside it (file_actions.cpp:1130-1134/1262: the legacy loop walks
    // <config>'s direct children and branches on tagName() == "protocols"
    // vs "car_models").
    pugi::xml_node car_models = doc.child("config").child("car_models");
    for (pugi::xml_node car_model : car_models.children("car_model"))
    {
        CarModelEntry entry;
        entry.make = text_or_empty(car_model, "make");
        entry.model = text_or_empty(car_model, "model");
        entry.version = text_or_empty(car_model, "version");
        entry.type = text_or_empty(car_model, "type");
        entry.kw = text_or_empty(car_model, "kw");
        entry.hp = text_or_empty(car_model, "hp");
        entry.fuel = text_or_empty(car_model, "fuel");
        entry.year = text_or_empty(car_model, "year");
        // Legacy reads this as the <protocol> child's *text content*
        // (car_model_data.text(), file_actions.cpp:1342), not an attribute
        // -- unlike <protocol name="..."> in the protocols section.
        entry.protocol_name = text_or_empty(car_model, "protocol");
        catalog.push_back(std::move(entry));
    }

    return catalog;
}

std::vector<ResolvedCarModel> resolve_car_models(const ProtocolCatalog& protocols, const CarModelCatalog& car_models)
{
    std::vector<ResolvedCarModel> resolved;
    resolved.reserve(car_models.size());
    for (const CarModelEntry& entry : car_models)
    {
        ResolvedCarModel row;
        row.make = entry.make;
        row.model = entry.model;
        row.version = entry.version;
        row.type = entry.type;
        row.kw = entry.kw;
        row.hp = entry.hp;
        row.fuel = entry.fuel;
        row.year = entry.year;
        row.protocol_name = entry.protocol_name;

        // First match wins. load_protocol_catalog rejects duplicate protocol
        // names outright, so at most one entry can match and the choice of
        // tie-break rule is not observable through that path.
        if (const auto matched = std::ranges::find(protocols, entry.protocol_name, &ProtocolEntry::protocol_name);
            matched != protocols.end())
        {
            row.protocol = *matched;
        }
        resolved.push_back(std::move(row));
    }
    return resolved;
}

std::optional<std::size_t> find_car_model_by_protocol_name(std::span<const ResolvedCarModel> resolved_car_models,
                                                           std::string_view flash_method)
{
    std::optional<std::size_t> matched_index;
    for (std::size_t i = 0; i < resolved_car_models.size(); ++i)
    {
        if (resolved_car_models[i].protocol_name == flash_method)
        {
            matched_index = i;
        }
    }
    return matched_index;
}

} // namespace fastecu::config
