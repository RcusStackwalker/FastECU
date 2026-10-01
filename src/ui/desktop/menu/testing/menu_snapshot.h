#pragma once

#include <string>

class QMenuBar;
class QToolBar;

namespace fastecu::ui::testing
{

// A stable, line-per-action text rendering of a menu bar and the actions of a
// toolbar, for golden comparison. Per action: text, portable shortcut text,
// checkable flag, whether the icon is non-null, and the tooltip. Submenus nest
// with two spaces per level; separators render as "---". Toolbar entries that
// host a widget (a QWidgetAction: comboboxes, buttons, spacers) are skipped:
// they belong to MainWindow, not to the menu.
std::string menu_snapshot(const QMenuBar& menubar, const QToolBar& toolbar);

} // namespace fastecu::ui::testing
