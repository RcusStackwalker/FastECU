#include "src/ui/desktop-quick/dashboard/dashboard_document_controller.h"

#include <utility>

#include <QFileInfo>

#include "src/backend/dashboard/dashboard_session_builder.h"

namespace fastecu::desktop_quick
{
namespace
{

QString to_qstring(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string first_card_id(const dashboard::DashboardDocument& document)
{
    if (document.cards.empty())
    {
        return {};
    }
    return document.cards.front().id;
}

} // namespace

DashboardDocumentController::DashboardDocumentController(dashboard::DashboardDocumentService& documents,
                                                         ISettings& settings, QObject *parent)
    : QObject(parent), documents_(documents), settings_(settings)
{
}

bool DashboardDocumentController::hasDocument() const
{
    return state_.document.has_value();
}

QString DashboardDocumentController::currentPath() const
{
    return to_qstring(state_.path);
}

QString DashboardDocumentController::displayName() const
{
    if (!state_.path.empty())
    {
        return QFileInfo(currentPath()).fileName();
    }
    if (!state_.document)
    {
        return {};
    }
    return to_qstring(state_.document->metadata.name);
}

bool DashboardDocumentController::isDirty() const
{
    return state_.dirty;
}

bool DashboardDocumentController::editingEnabled() const
{
    return connection_state_ == ConnectionState::Disconnected;
}

const std::optional<dashboard::DashboardDocument>& DashboardDocumentController::document() const
{
    return state_.document;
}

Status DashboardDocumentController::importDocument(std::string_view handle,
                                                   const dashboard::LegacyCdbgImportDefaults& defaults)
{
    const QString operation = QStringLiteral("Import dashboard");
    if (Status editable = requireEditingEnabled(operation); !editable.has_value())
    {
        return editable;
    }
    if (handle.empty())
    {
        return cancelledPath();
    }

    auto candidate = documents_.import_legacy_cdbg_catalog(handle, defaults);
    if (!candidate.has_value())
    {
        return reportError(operation, candidate.error());
    }

    Snapshot next{
        .document = std::move(*candidate),
        .path = {},
        .selected_card_id = {},
        .dirty = true,
    };
    commitReplacement(std::move(next));
    return {};
}

Status DashboardDocumentController::openDocument(std::string_view handle)
{
    return openDocument(handle, QStringLiteral("Open dashboard"));
}

Status DashboardDocumentController::openDocument(std::string_view handle, QString operation)
{
    if (Status editable = requireEditingEnabled(operation); !editable.has_value())
    {
        return editable;
    }
    if (handle.empty())
    {
        return cancelledPath();
    }

    auto candidate = documents_.load(handle);
    if (!candidate.has_value())
    {
        return reportError(operation, candidate.error());
    }
    if (auto prepared = dashboard::prepare_dashboard_session(*candidate); !prepared.has_value())
    {
        return reportError(operation, prepared.error());
    }

    const std::string path(handle);
    const std::string selected_card_id = first_card_id(*candidate);
    Snapshot next{
        .document = std::move(*candidate),
        .path = path,
        .selected_card_id = selected_card_id,
        .dirty = false,
    };
    commitReplacement(std::move(next));
    settings_.set(recentPathKey, path);
    return {};
}

Status DashboardDocumentController::save()
{
    const QString operation = QStringLiteral("Save dashboard");
    if (Status editable = requireEditingEnabled(operation); !editable.has_value())
    {
        return editable;
    }
    if (!state_.document)
    {
        return reportError(operation, Error{ErrorKind::InvalidConfig, "no dashboard document to save"});
    }
    if (state_.path.empty())
    {
        return reportError(operation, Error{ErrorKind::InvalidConfig, "choose a path before saving the dashboard"});
    }

    if (Status saved = documents_.save(state_.path, *state_.document); !saved.has_value())
    {
        return reportError(operation, saved.error());
    }

    settings_.set(recentPathKey, state_.path);
    if (state_.dirty)
    {
        state_.dirty = false;
        emit stateChanged();
    }
    return {};
}

Status DashboardDocumentController::saveAs(std::string_view handle)
{
    const QString operation = QStringLiteral("Save dashboard as");
    if (Status editable = requireEditingEnabled(operation); !editable.has_value())
    {
        return editable;
    }
    if (handle.empty())
    {
        return cancelledPath();
    }
    if (!state_.document)
    {
        return reportError(operation, Error{ErrorKind::InvalidConfig, "no dashboard document to save"});
    }

    if (Status saved = documents_.save(handle, *state_.document); !saved.has_value())
    {
        return reportError(operation, saved.error());
    }

    const std::string path(handle);
    const bool state_changed = state_.path != path || state_.dirty;
    state_.path = path;
    state_.dirty = false;
    settings_.set(recentPathKey, path);
    if (state_changed)
    {
        emit stateChanged();
    }
    return {};
}

Status DashboardDocumentController::restoreRecentDocument()
{
    const std::optional<std::string> recent = settings_.get(recentPathKey);
    if (!recent.has_value())
    {
        return {};
    }
    return openDocument(*recent, QStringLiteral("Restore recent dashboard"));
}

Status DashboardDocumentController::commitCandidate(dashboard::DashboardDocument candidate,
                                                    std::string selected_card_id)
{
    const QString operation = QStringLiteral("Edit dashboard");
    if (Status editable = requireEditingEnabled(operation); !editable.has_value())
    {
        return editable;
    }
    if (!state_.document)
    {
        return reportError(operation, Error{ErrorKind::InvalidConfig, "no dashboard document to edit"});
    }
    if (auto prepared = dashboard::prepare_dashboard_session(candidate); !prepared.has_value())
    {
        return reportError(operation, prepared.error());
    }
    if (candidate == *state_.document)
    {
        return {};
    }

    Snapshot next = state_;
    next.document = std::move(candidate);
    next.selected_card_id = std::move(selected_card_id);
    next.dirty = true;
    commitReplacement(std::move(next));
    return {};
}

void DashboardDocumentController::setConnectionState(ConnectionState state)
{
    if (state == connection_state_)
    {
        return;
    }
    connection_state_ = state;
    emit stateChanged();
}

Status DashboardDocumentController::requireEditingEnabled(const QString& operation)
{
    if (editingEnabled())
    {
        return {};
    }
    return reportError(operation, Error{ErrorKind::InvalidConfig, "disconnect before editing the dashboard"});
}

Status DashboardDocumentController::reportError(const QString& operation, Error error)
{
    emit errorOccurred(operation, to_qstring(error.detail));
    return std::unexpected(std::move(error));
}

Status DashboardDocumentController::cancelledPath()
{
    return fail(ErrorKind::Cancelled, "path selection cancelled");
}

void DashboardDocumentController::commitReplacement(Snapshot snapshot)
{
    state_ = std::move(snapshot);
    emit documentCommitted();
    emit stateChanged();
}

} // namespace fastecu::desktop_quick
