#ifndef QT_MENU_COMMAND_H
#define QT_MENU_COMMAND_H

#include "src/algorithms/menu/menu_command.h"

#include <QString>

inline MenuCommand menu_command_from_id(const QString& id)
{
    return menu_command_from_id(std::string_view(id.toUtf8().constData()));
}

inline QString menu_command_id_qt(MenuCommand command)
{
    return QString::fromStdString(menu_command_id(command));
}

#endif // QT_MENU_COMMAND_H
