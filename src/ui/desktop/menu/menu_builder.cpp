#include "src/ui/desktop/menu/menu_builder.h"

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QString>

#include <string>

namespace
{

QString qs(const std::string& text)
{
    return QString::fromStdString(text);
}

// `tooltip_prefix` carries the legacy asymmetry: a submenu's items are
// prefixed with the SUBMENU's name, a top-level item with its OWN name
// (file_actions.cpp:884 and :935).
QAction *make_action(const fastecu::config::MenuItem& item, const QString& tooltip_prefix, QToolBar *toolBar,
                     QSignalMapper *mapper, bool& toolbar_icon_set, QObject *parent)
{
    QAction *action = new QAction(qs(item.name), parent);
    action->setObjectName(qs(item.id));
    action->setShortcut(qs(item.shortcut));
    // Constructed unconditionally, exactly as legacy did: the "No icon"
    // sentinel yields a null QIcon rather than being skipped.
    action->setIcon(QIcon(qs(item.icon)));
    action->setIconVisibleInMenu(true);
    action->setToolTip(tooltip_prefix + "\n\n" + qs(item.tooltip));
    action->setCheckable(item.is_checkable());
    if (item.on_toolbar())
    {
        toolBar->addAction(action);
        toolbar_icon_set = true;
    }
    mapper->setMapping(action, action->objectName());
    QObject::connect(action, &QAction::triggered, mapper, qOverload<>(&QSignalMapper::map));
    return action;
}

} // namespace

QSignalMapper *build_menus(const fastecu::config::MenuDefinition& definition, QMenuBar *menubar, QToolBar *toolBar,
                           QObject *parent)
{
    QSignalMapper *mapper = new QSignalMapper(parent);

    for (const fastecu::config::Menu& menu : definition)
    {
        QMenu *main_menu = menubar->addMenu(qs(menu.name));
        // Reset per top-level menu, and shared with that menu's submenus:
        // one toolbar separator closes each menu that contributed an icon.
        bool toolbar_icon_set = false;

        for (const fastecu::config::MenuEntry& entry : menu.entries)
        {
            if (entry.is_submenu)
            {
                QMenu *sub_menu = main_menu->addMenu(qs(entry.submenu_name));
                for (const fastecu::config::MenuItem& item : entry.submenu_items)
                {
                    if (item.is_separator())
                    {
                        sub_menu->addSeparator();
                        continue;
                    }
                    sub_menu->addAction(
                        make_action(item, qs(entry.submenu_name), toolBar, mapper, toolbar_icon_set, parent));
                }
                continue;
            }

            if (entry.item.is_separator())
            {
                main_menu->addSeparator();
                continue;
            }
            main_menu->addAction(
                make_action(entry.item, qs(entry.item.name), toolBar, mapper, toolbar_icon_set, parent));
        }

        if (toolbar_icon_set)
        {
            toolBar->addSeparator();
        }
    }

    return mapper;
}
