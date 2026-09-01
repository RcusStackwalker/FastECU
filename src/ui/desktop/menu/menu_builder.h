#pragma once
#include <QMenuBar>
#include <QObject>
#include <QSignalMapper>
#include <QToolBar>

#include "src/backend/config/menu_definition.h"

// Builds the application menu bar and tool bar from a parsed MenuDefinition.
// This is the widget-construction half of the former
// FileActions::read_menu_file; the parsing half is
// fastecu::config::load_menu_definition.
//
// Every created QAction and the returned QSignalMapper are parented to
// `parent`, which owns them. Each non-separator action is mapped to its
// MenuItem::id, so the caller connects QSignalMapper::mappedString once to
// receive every menu command as a string.
QSignalMapper *build_menus(const fastecu::config::MenuDefinition& definition, QMenuBar *menubar, QToolBar *toolBar,
                           QObject *parent);
