#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QString>
#include <QStringList>

#include <cstdlib>

#include "src/platform/desktop/common/connection/desktop_connection_service.h"
#include "src/platform/desktop/common/connection/j2534_adapter_provider.h"
#include "src/platform/desktop/common/connection/socketcan_adapter_provider.h"
#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OmniHaste"));
    QCoreApplication::setApplicationName(QStringLiteral("OmniHaste"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    fastecu::desktop::connection::J2534AdapterProvider j2534_provider;
    fastecu::desktop::connection::SocketCanAdapterProvider socketcan_provider;
    fastecu::desktop::connection::DesktopConnectionService connection_service({&j2534_provider, &socketcan_provider});
    fastecu::desktop::logging::LoggingEngine logging_engine;
    fastecu::desktop_quick::DashboardConnectionController dashboard_connection(connection_service, logging_engine);

    QQmlApplicationEngine engine;
    if (!fastecu::desktop_quick::load_root(engine, dashboard_connection))
    {
        return EXIT_FAILURE;
    }
    if (QCoreApplication::arguments().contains(QStringLiteral("--smoke-test")))
    {
        return EXIT_SUCCESS;
    }
    return QGuiApplication::exec();
}
