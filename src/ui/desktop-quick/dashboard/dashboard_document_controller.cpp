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

std::string to_string(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

dashboard::LegacyCdbgImportDefaults import_defaults(const QString& path)
{
    return dashboard::LegacyCdbgImportDefaults{
        .document_name = to_string(QFileInfo(path).completeBaseName()),
        .bitrate = 500000,
        .identifier_width = dashboard::CanIdentifierWidth::Standard,
        .stream_instance = 0,
        .sampling_interval_ms = 50,
        .retry = dashboard::RetryPolicy{100, 3, 3, 250},
    };
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
    const QString operation = QStringLiteral("Restore recent dashboard");
    if (Status editable = requireEditingEnabled(operation); !editable.has_value())
    {
        return editable;
    }
    const std::optional<std::string> recent = settings_.get(recentPathKey);
    if (!recent.has_value())
    {
        return {};
    }
    return openDocument(*recent, operation);
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

void DashboardDocumentController::requestImport()
{
    requestAction(PendingDocumentAction::Import, QStringLiteral("Import dashboard"));
}

void DashboardDocumentController::requestOpen()
{
    requestAction(PendingDocumentAction::Open, QStringLiteral("Open dashboard"));
}

void DashboardDocumentController::requestExit()
{
    requestAction(PendingDocumentAction::Exit, QStringLiteral("Exit"));
}

void DashboardDocumentController::resolveUnsaved(UnsavedDecision decision)
{
    if (pending_action_ == PendingDocumentAction::None)
    {
        return;
    }

    switch (decision)
    {
    case UnsavedDecision::Cancel:
        pending_action_ = PendingDocumentAction::None;
        return;
    case UnsavedDecision::Discard:
        continuePendingAction();
        return;
    case UnsavedDecision::Save:
        if (state_.path.empty())
        {
            emit savePathRequested();
            return;
        }
        if (save().has_value())
        {
            continuePendingAction();
        }
        return;
    }
}

void DashboardDocumentController::cancelPathRequest()
{
    pending_action_ = PendingDocumentAction::None;
}

void DashboardDocumentController::completeImportPath(const QString& path)
{
    if (pending_action_ != PendingDocumentAction::Import)
    {
        return;
    }
    if (path.isEmpty())
    {
        cancelPathRequest();
        return;
    }

    const std::string handle = to_string(path);
    pending_action_ = PendingDocumentAction::None;
    importDocument(handle, import_defaults(path));
}

void DashboardDocumentController::completeOpenPath(const QString& path)
{
    if (pending_action_ != PendingDocumentAction::Open)
    {
        return;
    }
    if (path.isEmpty())
    {
        cancelPathRequest();
        return;
    }

    pending_action_ = PendingDocumentAction::None;
    openDocument(to_string(path));
}

void DashboardDocumentController::completeSavePath(const QString& path)
{
    if (pending_action_ == PendingDocumentAction::None)
    {
        return;
    }
    if (path.isEmpty())
    {
        cancelPathRequest();
        return;
    }

    if (saveAs(to_string(path)).has_value())
    {
        continuePendingAction();
    }
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
    emit errorOccurred(operation, to_qstring(error.detail), error.kind);
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

void DashboardDocumentController::requestAction(PendingDocumentAction action, const QString& operation)
{
    if (pending_action_ != PendingDocumentAction::None)
    {
        static_cast<void>(
            reportError(operation, Error{ErrorKind::InvalidConfig, "a document action is already pending"}));
        return;
    }
    if (action != PendingDocumentAction::Exit)
    {
        if (Status editable = requireEditingEnabled(operation); !editable.has_value())
        {
            return;
        }
    }

    pending_action_ = action;
    if (state_.dirty)
    {
        emit unsavedDecisionRequested();
        return;
    }
    continuePendingAction();
}

void DashboardDocumentController::continuePendingAction()
{
    switch (pending_action_)
    {
    case PendingDocumentAction::None:
        return;
    case PendingDocumentAction::Import:
        emit importPathRequested();
        return;
    case PendingDocumentAction::Open:
        emit openPathRequested();
        return;
    case PendingDocumentAction::Exit:
        pending_action_ = PendingDocumentAction::None;
        emit exitApproved();
        return;
    }
}

} // namespace fastecu::desktop_quick
