#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <QObject>
#include <QString>

#include "src/backend/dashboard/dashboard_document_service.h"
#include "src/backend/ports/settings.h"
#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"

namespace fastecu::desktop_quick
{

class DashboardDocumentController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY stateChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY stateChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY stateChanged)
    Q_PROPERTY(bool editingEnabled READ editingEnabled NOTIFY stateChanged)

  public:
    static constexpr std::string_view recentPathKey = "desktop-quick/recent-dashboard";

    DashboardDocumentController(dashboard::DashboardDocumentService& documents, ISettings& settings,
                                QObject *parent = nullptr);

    bool hasDocument() const;
    QString currentPath() const;
    QString displayName() const;
    bool isDirty() const;
    bool editingEnabled() const;
    const std::optional<dashboard::DashboardDocument>& document() const;

    Status importDocument(std::string_view handle, const dashboard::LegacyCdbgImportDefaults& defaults);
    Status openDocument(std::string_view handle);
    Status save();
    Status saveAs(std::string_view handle);
    Status restoreRecentDocument();
    Status commitCandidate(dashboard::DashboardDocument candidate, std::string selected_card_id);
    void setConnectionState(ConnectionState state);

  signals:
    void documentCommitted();
    void stateChanged();
    void errorOccurred(QString operation, QString detail);

  private:
    struct Snapshot
    {
        std::optional<dashboard::DashboardDocument> document;
        std::string path;
        std::string selected_card_id;
        bool dirty = false;
    };

    Status openDocument(std::string_view handle, QString operation);
    Status requireEditingEnabled(const QString& operation);
    Status reportError(const QString& operation, Error error);
    static Status cancelledPath();
    void commitReplacement(Snapshot snapshot);

    dashboard::DashboardDocumentService& documents_;
    ISettings& settings_;
    Snapshot state_;
    ConnectionState connection_state_ = ConnectionState::Disconnected;
};

} // namespace fastecu::desktop_quick
