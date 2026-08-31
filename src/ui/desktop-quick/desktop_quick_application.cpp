#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>
#include <QUrl>

#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_document_controller.h"

namespace fastecu::desktop_quick
{

bool load_root(QQmlApplicationEngine& engine, DashboardConnectionController& dashboard_connection,
               DashboardController& dashboard_presentation)
{
    static const int dashboard_card_model_type = qmlRegisterUncreatableType<DashboardCardModel>(
        "OmniHaste.Dashboard", 1, 0, "DashboardCardModel",
        "DashboardCardModel instances are owned by dashboardPresentation");
    static_cast<void>(dashboard_card_model_type);
    static const int dashboard_document_controller_type = qmlRegisterUncreatableType<DashboardDocumentController>(
        "OmniHaste.Dashboard", 1, 0, "DashboardDocumentController",
        "DashboardDocumentController instances are owned by the application");
    static_cast<void>(dashboard_document_controller_type);
    QObject::connect(&dashboard_presentation, &DashboardController::diagnostic, &engine,
                     [](const QString& message) { qWarning().noquote() << message; });
    engine.rootContext()->setContextProperty(QStringLiteral("dashboardConnection"), &dashboard_connection);
    engine.rootContext()->setContextProperty(QStringLiteral("dashboardPresentation"), &dashboard_presentation);
    engine.load(QUrl{QStringLiteral("qrc:/omnihaste/qml/Main.qml")});
    return !engine.rootObjects().isEmpty();
}

} // namespace fastecu::desktop_quick
