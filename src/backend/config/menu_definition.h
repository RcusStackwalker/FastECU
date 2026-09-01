#pragma once
#include <string>
#include <vector>

#include "src/backend/config/config_paths.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::config
{

// One <menuitem> element from menu.cfg. Every field keeps the sentinel
// default the legacy QDomElement::attribute(name, default) calls used
// (file_actions.cpp:796-1005) rather than an empty string: the builder
// branches on the literal "true", and shipped entries do omit attributes.
struct MenuItem
{
    std::string name{"No name"};
    std::string id{"No id"};
    std::string checkable{"No checkable"};
    std::string shortcut{"No shortcut"};
    std::string toolbar{"No toolbar"};
    std::string icon{"No icon"};
    std::string tooltip{"No tooltip"};

    // Legacy tests the literal string in all three cases; anything else,
    // including the "No ..." sentinels, is false.
    bool is_separator() const
    {
        return name == "Separator";
    }
    bool is_checkable() const
    {
        return checkable == "true";
    }
    bool on_toolbar() const
    {
        return toolbar == "true";
    }

    bool operator==(const MenuItem&) const = default;
};

// One ordered child of a top-level <menu>: either a <menuitem> or a nested
// <menu>. The legacy grammar is exactly two levels deep -- a submenu's
// children are only <menuitem> -- so this is a flat pair rather than a
// recursive node. Order matters: a submenu and the items around it appear in
// the menu in document order.
struct MenuEntry
{
    bool is_submenu{false};
    std::string submenu_name;            // set when is_submenu
    std::vector<MenuItem> submenu_items; // set when is_submenu
    MenuItem item;                       // set when !is_submenu

    bool operator==(const MenuEntry&) const = default;
};

struct Menu
{
    std::string name{"No name"};
    std::vector<MenuEntry> entries;

    bool operator==(const Menu&) const = default;
};

using MenuDefinition = std::vector<Menu>;

// Replaces the parsing half of FileActions::read_menu_file. Reads
// paths.menu_file through the repository and returns the
// <ecu_menu_definitions> section.
//
// <popup_menu_definitions> is deliberately ignored: its legacy handler is
// commented out (file_actions.cpp:966-999), so parsing it would add menus
// that do not exist today. A file with no <config> or no
// <ecu_menu_definitions> yields an empty MenuDefinition rather than an
// error -- legacy built no menus in that case and reported nothing.
//
// The section is reached via doc.child("config").child("ecu_menu_definitions"),
// which requires the root element to be literally named "config" and takes
// only the first such section -- unlike legacy's QDomDocument::documentElement(),
// which was purely positional and ignored the root's tag name. A renamed
// root or a second <ecu_menu_definitions> would therefore yield an empty
// menu bar with no diagnostic, same as the "missing section" case above.
// Not fixed here: ConfigPaths::menu_file resolves under a per-version
// resource directory that always ships a <config> root, and the golden test
// pins the shipped file, so practical exposure is nil.
Result<MenuDefinition> load_menu_definition(const ConfigPaths& paths, IFileRepository& file_repository);

} // namespace fastecu::config
