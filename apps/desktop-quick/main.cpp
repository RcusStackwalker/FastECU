#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QString>
#include <QStringList>

#include <cstdlib>
#include <memory>
#include <utility>

#include "src/backend/dashboard/dashboard_document_service.h"
#include "src/platform/desktop/common/connection/desktop_connection_service.h"
#include "src/platform/desktop/common/connection/j2534_adapter_provider.h"
#include "src/platform/desktop/common/connection/socketcan_adapter_provider.h"
#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/ui/desktop-quick/dashboard/bundled_dashboard_loader.h"
#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_controller.h"

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
    auto dashboard_logging = fastecu::desktop_quick::detail::make_logging_engine_bridge(logging_engine);
    fastecu::desktop_quick::DashboardConnectionController dashboard_connection(connection_service, logging_engine);
    QtFileRepository file_repository;
    QtAtomicFileWriter file_writer;
    fastecu::dashboard::DashboardDocumentService document_service(file_repository, file_writer);
    QtClock clock;

    std::unique_ptr<fastecu::desktop_quick::DashboardController> dashboard_presentation;
    auto loaded_document = fastecu::desktop_quick::load_bundled_colt_dashboard(document_service);
    if (loaded_document.has_value())
    {
        fastecu::dashboard::DashboardDocument document = std::move(*loaded_document);
        dashboard_connection.setDocument(document);
        dashboard_presentation = std::make_unique<fastecu::desktop_quick::DashboardController>(
            document, *dashboard_logging, dashboard_connection, clock);
    }
    else
    {
        dashboard_presentation = fastecu::desktop_quick::DashboardController::fromLoadError(
            QString::fromStdString(loaded_document.error().detail), *dashboard_logging, dashboard_connection, clock);
    }

    QQmlApplicationEngine engine;
    if (!fastecu::desktop_quick::load_root(engine, dashboard_connection, *dashboard_presentation))
    {
        return EXIT_FAILURE;
    }
    if (QCoreApplication::arguments().contains(QStringLiteral("--smoke-test")))
    {
        return EXIT_SUCCESS;
    }
    return QGuiApplication::exec();
}
