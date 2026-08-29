#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QString>
#include <QUrl>

#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_controller.h"

namespace fastecu::desktop_quick
{

bool load_root(QQmlApplicationEngine& engine, DashboardConnectionController& dashboard_connection,
               DashboardController& dashboard_presentation)
{
    engine.rootContext()->setContextProperty(QStringLiteral("dashboardConnection"), &dashboard_connection);
    engine.rootContext()->setContextProperty(QStringLiteral("dashboardPresentation"), &dashboard_presentation);
    engine.load(QUrl{QStringLiteral("qrc:/omnihaste/qml/Main.qml")});
    return !engine.rootObjects().isEmpty();
}

} // namespace fastecu::desktop_quick
