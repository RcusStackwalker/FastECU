#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QQmlApplicationEngine>
#include <QString>
#include <QUrl>

namespace fastecu::desktop_quick
{

bool load_root(QQmlApplicationEngine& engine)
{
    engine.load(QUrl{QStringLiteral("qrc:/omnihaste/qml/Main.qml")});
    return !engine.rootObjects().isEmpty();
}

} // namespace fastecu::desktop_quick
