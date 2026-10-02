#include "src/ui/desktop/menu/testing/menu_snapshot.h"

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QString>
#include <QStringList>
#include <QToolBar>
#include <QWidgetAction>

#include <string>

namespace
{

QString shortcut_text(const QAction& action)
{
    QStringList parts;
    for (const QKeySequence& sequence : action.shortcuts())
    {
        parts << sequence.toString(QKeySequence::PortableText);
    }
    return parts.join(QLatin1Char(';'));
}

QString tooltip_text(QString tooltip)
{
    tooltip.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return tooltip;
}

void append_line(std::string& out, int depth, const QString& text)
{
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    out += text.toStdString();
    out += '\n';
}

void append_action(std::string& out, int depth, const QAction& action)
{
    if (action.isSeparator())
    {
        append_line(out, depth, QStringLiteral("---"));
        return;
    }
    append_line(
        out, depth,
        QStringLiteral("%1 | keys=%2 | checkable=%3 | icon=%4 | tip=%5")
            .arg(action.text(), shortcut_text(action), action.isCheckable() ? QStringLiteral("1") : QStringLiteral("0"),
                 action.icon().isNull() ? QStringLiteral("0") : QStringLiteral("1"), tooltip_text(action.toolTip())));
}

void append_menu(std::string& out, int depth, const QMenu& menu)
{
    append_line(out, depth, QStringLiteral("[%1]").arg(menu.title()));
    for (const QAction *action : menu.actions())
    {
        if (action->menu() != nullptr)
        {
            append_menu(out, depth + 1, *action->menu());
            continue;
        }
        append_action(out, depth + 1, *action);
    }
}

} // namespace

namespace fastecu::ui::testing
{

std::string menu_snapshot(const QMenuBar& menubar, const QToolBar& toolbar)
{
    std::string out;
    for (const QAction *top : menubar.actions())
    {
        if (top->menu() != nullptr)
        {
            append_menu(out, 0, *top->menu());
        }
    }
    append_line(out, 0, QStringLiteral("[toolbar]"));
    for (const QAction *action : toolbar.actions())
    {
        if (qobject_cast<const QWidgetAction *>(action) != nullptr)
        {
            break;
        }
        append_action(out, 1, *action);
    }
    return out;
}

} // namespace fastecu::ui::testing
