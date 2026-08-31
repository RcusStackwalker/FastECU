#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QString>
#include <QStringList>

#include <cstdlib>
#include <optional>

#include "src/backend/dashboard/dashboard_document_service.h"
#include "src/platform/desktop/common/connection/desktop_connection_service.h"
#include "src/platform/desktop/common/connection/j2534_adapter_provider.h"
#include "src/platform/desktop/common/connection/socketcan_adapter_provider.h"
#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_settings.h"
#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_document_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_editor_model.h"

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
    QtSettings settings;
    fastecu::desktop_quick::DashboardDocumentController dashboard_documents(document_service, settings);
    fastecu::desktop_quick::DashboardEditorModel dashboard_editor(dashboard_documents);
    QtClock clock;
    fastecu::desktop_quick::DashboardController dashboard_presentation(std::nullopt, *dashboard_logging,
                                                                       dashboard_connection, clock);

    const fastecu::Status restored = dashboard_documents.restoreRecentDocument();

    QQmlApplicationEngine engine;
    if (!fastecu::desktop_quick::load_root(engine, dashboard_connection, dashboard_presentation, dashboard_documents,
                                           dashboard_editor))
    {
        return EXIT_FAILURE;
    }
    if (!restored.has_value())
    {
        dashboard_documents.errorOccurred(QStringLiteral("Restore recent dashboard"),
                                          QString::fromStdString(restored.error().detail), restored.error().kind);
    }
    if (QCoreApplication::arguments().contains(QStringLiteral("--smoke-test")))
    {
        return EXIT_SUCCESS;
    }
    return QGuiApplication::exec();
}
