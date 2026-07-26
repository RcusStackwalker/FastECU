#include "src/backend/config/car_model_catalog.h"

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
    pugi::xml_parse_result parsed = doc.load_buffer(bytes->data(), bytes->size());
    if (!parsed)
    {
        return fail(ErrorKind::InvalidConfig, std::string("protocols parse error: ") + parsed.description());
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

} // namespace fastecu::config
