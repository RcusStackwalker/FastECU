#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QCloseEvent>
#include <QDebug>
#include <QEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QString>
#include <QUrl>

#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_document_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_editor_model.h"

namespace fastecu::desktop_quick
{
namespace
{

class CloseRequestHandler final : public QObject
{
  public:
    CloseRequestHandler(QQuickWindow& window, DashboardDocumentController& documents)
        : QObject(&window), window_(window), documents_(documents)
    {
        window_.installEventFilter(this);
        connect(&documents_, &DashboardDocumentController::exitApproved, this,
                [this]
                {
                    exit_approved_ = true;
                    window_.close();
                });
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != &window_ || event->type() != QEvent::Close || exit_approved_)
        {
            return false;
        }
        static_cast<QCloseEvent *>(event)->ignore();
        documents_.requestExit();
        return true;
    }

  private:
    QQuickWindow& window_;
    DashboardDocumentController& documents_;
    bool exit_approved_ = false;
};

} // namespace

bool load_root(QQmlApplicationEngine& engine, DashboardConnectionController& dashboard_connection,
               DashboardController& dashboard_presentation, DashboardDocumentController& dashboard_documents,
               DashboardEditorModel& dashboard_editor)
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
    const auto fan_out_committed_document = [&dashboard_connection, &dashboard_presentation, &dashboard_documents]
    {
        const std::optional<dashboard::DashboardDocument> committed_document = dashboard_documents.document();
        dashboard_presentation.setDocument(committed_document);
        dashboard_connection.setDocument(committed_document);
    };
    QObject::connect(&dashboard_documents, &DashboardDocumentController::documentCommitted, &engine,
                     fan_out_committed_document);
    QObject::connect(&dashboard_connection, &DashboardConnectionController::stateChanged, &engine,
                     [&dashboard_connection, &dashboard_documents]
                     { dashboard_documents.setConnectionState(dashboard_connection.state()); });
    fan_out_committed_document();
    dashboard_documents.setConnectionState(dashboard_connection.state());
    engine.rootContext()->setContextProperty(QStringLiteral("dashboardConnection"), &dashboard_connection);
    engine.rootContext()->setContextProperty(QStringLiteral("dashboardPresentation"), &dashboard_presentation);
    engine.rootContext()->setContextProperty(QStringLiteral("dashboardDocuments"), &dashboard_documents);
    engine.rootContext()->setContextProperty(QStringLiteral("dashboardEditor"), &dashboard_editor);
    engine.load(QUrl{QStringLiteral("qrc:/omnihaste/qml/Main.qml")});
    if (engine.rootObjects().isEmpty())
    {
        return false;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    if (window == nullptr)
    {
        return false;
    }
    new CloseRequestHandler(*window, dashboard_documents);
    return true;
}

} // namespace fastecu::desktop_quick
