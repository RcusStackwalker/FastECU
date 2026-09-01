#include "src/backend/config/menu_definition.h"

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#include <pugixml.hpp>

namespace fastecu::config
{
namespace
{

MenuItem parse_item(pugi::xml_node node)
{
    MenuItem item;
    item.name = node.attribute("name").as_string("No name");
    item.id = node.attribute("id").as_string("No id");
    item.checkable = node.attribute("checkable").as_string("No checkable");
    item.shortcut = node.attribute("shortcut").as_string("No shortcut");
    item.toolbar = node.attribute("toolbar").as_string("No toolbar");
    item.icon = node.attribute("icon").as_string("No icon");
    item.tooltip = node.attribute("tooltip").as_string("No tooltip");
    return item;
}

MenuEntry parse_submenu(pugi::xml_node node)
{
    MenuEntry entry;
    entry.is_submenu = true;
    entry.submenu_name = node.attribute("name").as_string("No name");
    for (pugi::xml_node sub_item : node.children("menuitem"))
    {
        entry.submenu_items.push_back(parse_item(sub_item));
    }
    return entry;
}

} // namespace

Result<MenuDefinition> load_menu_definition(const ConfigPaths& paths, IFileRepository& file_repository)
{
    Result<std::vector<std::uint8_t>> bytes = file_repository.read(paths.menu_file);
    if (!bytes.has_value())
    {
        return std::unexpected(bytes.error());
    }

    pugi::xml_document doc;
    if (pugi::xml_parse_result parsed = doc.load_buffer(bytes->data(), bytes->size()); !parsed)
    {
        return fail(ErrorKind::InvalidConfig, std::format("menu parse error: {}", parsed.description()));
    }

    MenuDefinition definition;
    // An absent <config> or <ecu_menu_definitions> yields empty node sets
    // here, so this loop simply does not run -- matching legacy, which
    // built no menus and reported no error.
    for (pugi::xml_node menu : doc.child("config").child("ecu_menu_definitions").children("menu"))
    {
        Menu parsed_menu;
        parsed_menu.name = menu.attribute("name").as_string("No name");
        // Iterate every child node, not children("menuitem"), so that a
        // submenu and the items around it keep document order. Comments and
        // text nodes report an empty name() and fall through both branches.
        for (pugi::xml_node child : menu.children())
        {
            const std::string_view tag(child.name());
            if (tag == "menu")
            {
                parsed_menu.entries.push_back(parse_submenu(child));
            }
            else if (tag == "menuitem")
            {
                MenuEntry entry;
                entry.item = parse_item(child);
                parsed_menu.entries.push_back(std::move(entry));
            }
        }
        definition.push_back(std::move(parsed_menu));
    }
    return definition;
}

} // namespace fastecu::config
