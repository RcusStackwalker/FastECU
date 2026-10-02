#pragma once

#include <string>

class QMenuBar;
class QToolBar;

namespace fastecu::ui::testing
{

// A stable, line-per-action text rendering of a menu bar and the actions of a
// toolbar, for golden comparison. Per action: text, portable shortcut text,
// checkable flag, whether the icon is non-null, and the tooltip. Submenus nest
// with two spaces per level; separators render as "---". The toolbar section
// ends at the first entry that hosts a widget (a QWidgetAction: comboboxes,
// buttons, spacers): from there on the toolbar belongs to MainWindow, which
// appends those widgets and the separators between them in code.
std::string menu_snapshot(const QMenuBar& menubar, const QToolBar& toolbar);

} // namespace fastecu::ui::testing
