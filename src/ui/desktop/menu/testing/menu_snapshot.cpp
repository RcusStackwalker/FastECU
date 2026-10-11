#include "src/ui/desktop/menu/testing/menu_snapshot.h"

#include <QAction>
#include <QIcon>
#include <QKeySequence>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QToolBar>
#include <QWidgetAction>

#include <cstddef>
#include <string>

namespace
{

QString shortcutText(const QAction& action)
{
    QStringList parts;
    for (const QKeySequence& sequence : action.shortcuts())
    {
        parts << sequence.toString(QKeySequence::PortableText);
    }
    return parts.join(QLatin1Char(';'));
}

QString tooltipText(QString tooltip)
{
    tooltip.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return tooltip;
}

void appendLine(std::string& out, int depth, const QString& text)
{
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    out += text.toStdString();
    out += '\n';
}

void appendAction(std::string& out, int depth, const QAction& action)
{
    if (action.isSeparator())
    {
        appendLine(out, depth, QStringLiteral("---"));
        return;
    }
    appendLine(
        out, depth,
        QStringLiteral("%1 | keys=%2 | checkable=%3 | icon=%4 | tip=%5")
            .arg(action.text(), shortcutText(action), action.isCheckable() ? QStringLiteral("1") : QStringLiteral("0"),
                 action.icon().isNull() ? QStringLiteral("0") : QStringLiteral("1"), tooltipText(action.toolTip())));
}

void appendMenu(std::string& out, int depth, const QMenu& menu)
{
    appendLine(out, depth, QStringLiteral("[%1]").arg(menu.title()));
    for (const QAction *action : menu.actions())
    {
        if (action->menu() != nullptr)
        {
            appendMenu(out, depth + 1, *action->menu());
            continue;
        }
        appendAction(out, depth + 1, *action);
    }
}

} // namespace

namespace fastecu::ui::testing
{

std::string menuSnapshot(const QMenuBar& menubar, const QToolBar& toolbar)
{
    std::string out;
    for (const QAction *top : menubar.actions())
    {
        if (top->menu() != nullptr)
        {
            appendMenu(out, 0, *top->menu());
        }
    }
    appendLine(out, 0, QStringLiteral("[toolbar]"));
    for (const QAction *action : toolbar.actions())
    {
        if (qobject_cast<const QWidgetAction *>(action) != nullptr)
        {
            break;
        }
        appendAction(out, 1, *action);
    }
    return out;
}

} // namespace fastecu::ui::testing
