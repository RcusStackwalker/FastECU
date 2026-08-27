#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace fastecu::desktop_quick
{
namespace
{

QString kind_label(dashboard::AdapterKind kind)
{
    switch (kind)
    {
    case dashboard::AdapterKind::J2534:
        return QStringLiteral("J2534");
    case dashboard::AdapterKind::SocketCan:
        return QStringLiteral("SocketCAN");
    }
    return QStringLiteral("Unknown adapter");
}

class DesktopConnectionPreparationService final : public IConnectionPreparationService
{
  public:
    explicit DesktopConnectionPreparationService(connection::DesktopConnectionService& service) : service_(service)
    {
    }

    connection::ConnectionPreparationOutcome prepare_run(const dashboard::DashboardDocument& document,
                                                         std::optional<connection::AdapterSelection> selection) override
    {
        return service_.prepare_run(document, std::move(selection));
    }

    Result<connection::AdapterDiscoverySnapshot> refresh() override
    {
        return service_.refresh();
    }

  private:
    connection::DesktopConnectionService& service_;
};

class DesktopLoggingEngineBridge final : public ILoggingEngine
{
  public:
    explicit DesktopLoggingEngineBridge(desktop::logging::LoggingEngine& engine) : engine_(engine)
    {
        connect(&engine_, &desktop::logging::LoggingEngine::statusChanged, this, &ILoggingEngine::statusChanged);
        connect(&engine_, &desktop::logging::LoggingEngine::sessionEnded, this, &ILoggingEngine::sessionEnded);
    }

    Status start(desktop::logging::LoggingRun run) override
    {
        return engine_.start(std::move(run));
    }
    void stop() override
    {
        engine_.stop();
    }
    bool isRunning() const override
    {
        return engine_.isRunning();
    }

  private:
    desktop::logging::LoggingEngine& engine_;
};

} // namespace

AdapterCandidateModel::AdapterCandidateModel(QObject *parent) : QAbstractListModel(parent)
{
}

int AdapterCandidateModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(snapshot_.candidates.size());
}

QVariant AdapterCandidateModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
    {
        return {};
    }
    const connection::LocalAdapterDescriptor& candidate = snapshot_.candidates[static_cast<std::size_t>(index.row())];
    switch (role)
    {
    case CandidateIdRole:
        return QString::fromStdString(candidate.candidate_id);
    case LabelRole:
        return QString::fromStdString(candidate.label);
    case KindLabelRole:
        return kind_label(candidate.kind);
    default:
        return {};
    }
}

QHash<int, QByteArray> AdapterCandidateModel::roleNames() const
{
    return {{CandidateIdRole, "candidateId"}, {LabelRole, "label"}, {KindLabelRole, "kindLabel"}};
}

void AdapterCandidateModel::replace(connection::AdapterDiscoverySnapshot snapshot)
{
    beginResetModel();
    snapshot_ = std::move(snapshot);
    endResetModel();
}

bool AdapterCandidateModel::contains(const QString& candidate_id) const
{
    return std::any_of(snapshot_.candidates.begin(), snapshot_.candidates.end(), [&](const auto& candidate)
                       { return QString::fromStdString(candidate.candidate_id) == candidate_id; });
}

std::uint64_t AdapterCandidateModel::generation() const
{
    return snapshot_.generation;
}

DashboardConnectionController::DashboardConnectionController(IConnectionPreparationService& preparation,
                                                             ILoggingEngine& engine, QObject *parent)
    : QObject(parent), preparation_(preparation), engine_(engine), candidates_(this)
{
    qRegisterMetaType<ConnectionState>();
    connect(&engine_, &ILoggingEngine::statusChanged, this, &DashboardConnectionController::handleStatus);
    connect(&engine_, &ILoggingEngine::sessionEnded, this, &DashboardConnectionController::handleCompletion);
}

DashboardConnectionController::DashboardConnectionController(connection::DesktopConnectionService& preparation,
                                                             desktop::logging::LoggingEngine& engine, QObject *parent)
    : QObject(parent), owned_preparation_(std::make_unique<DesktopConnectionPreparationService>(preparation)),
      owned_engine_(std::make_unique<DesktopLoggingEngineBridge>(engine)), preparation_(*owned_preparation_),
      engine_(*owned_engine_), candidates_(this)
{
    qRegisterMetaType<ConnectionState>();
    connect(&engine_, &ILoggingEngine::statusChanged, this, &DashboardConnectionController::handleStatus);
    connect(&engine_, &ILoggingEngine::sessionEnded, this, &DashboardConnectionController::handleCompletion);
}

DashboardConnectionController::~DashboardConnectionController()
{
    destroying_ = true;
    const bool should_stop = engine_started_;
    ++operation_generation_;
    active_run_generation_ = 0;
    engine_started_ = false;
    stop_completion_pending_ = false;
    if (should_stop)
    {
        engine_.stop();
    }
}

ConnectionState DashboardConnectionController::state() const
{
    return state_;
}
QString DashboardConnectionController::statusText() const
{
    return status_text_;
}
QString DashboardConnectionController::technicalDetail() const
{
    return technical_detail_;
}
QString DashboardConnectionController::selectedAdapterLabel() const
{
    return selected_adapter_label_;
}
QAbstractItemModel *DashboardConnectionController::candidates()
{
    return &candidates_;
}
qulonglong DashboardConnectionController::discoveryGeneration() const
{
    return static_cast<qulonglong>(candidates_.generation());
}
bool DashboardConnectionController::canConnect() const
{
    return hasUsableDocument() && (state_ == ConnectionState::Disconnected || state_ == ConnectionState::Failed);
}
bool DashboardConnectionController::canDisconnect() const
{
    return state_ == ConnectionState::Connecting || state_ == ConnectionState::Running ||
           state_ == ConnectionState::CarNotResponding;
}
bool DashboardConnectionController::needsAdapterSelection() const
{
    return state_ == ConnectionState::AdapterSelectionRequired;
}

void DashboardConnectionController::setDocument(std::optional<dashboard::DashboardDocument> document)
{
    const bool was_connectable = canConnect();
    document_ = std::move(document);
    if (was_connectable != canConnect())
    {
        emit stateChanged();
    }
}

void DashboardConnectionController::connectDashboard()
{
    if (!canConnect())
    {
        return;
    }
    const std::uint64_t operation_generation = ++operation_generation_;
    setPresentation(QStringLiteral("Connecting"));
    if (operation_generation != operation_generation_)
    {
        return;
    }
    setState(ConnectionState::Connecting);
    prepare(std::nullopt, operation_generation);
}

void DashboardConnectionController::connectWithAdapter(const QString& candidate_id)
{
    if (!hasUsableDocument() || state_ != ConnectionState::AdapterSelectionRequired ||
        !candidates_.contains(candidate_id))
    {
        return;
    }
    const std::uint64_t operation_generation = ++operation_generation_;
    setPresentation(QStringLiteral("Connecting"));
    if (operation_generation != operation_generation_)
    {
        return;
    }
    setState(ConnectionState::Connecting);
    prepare(
        connection::AdapterSelection{.generation = discoveryGeneration(), .candidate_id = candidate_id.toStdString()},
        operation_generation);
}

void DashboardConnectionController::refreshAdapters()
{
    if (state_ != ConnectionState::Disconnected && state_ != ConnectionState::Failed &&
        state_ != ConnectionState::AdapterSelectionRequired)
    {
        return;
    }
    Result<connection::AdapterDiscoverySnapshot> refreshed = preparation_.refresh();
    if (!refreshed)
    {
        fail(QStringLiteral("Unable to refresh adapters"), QString::fromStdString(refreshed.error().detail));
        return;
    }
    candidates_.replace(std::move(*refreshed));
    emit presentationChanged();
}

void DashboardConnectionController::disconnectDashboard()
{
    if (!canDisconnect())
    {
        return;
    }
    const bool should_stop = engine_started_ || active_run_generation_ != 0;
    const std::uint64_t operation_generation = ++operation_generation_;
    active_run_generation_ = 0;
    engine_started_ = false;
    stop_completion_pending_ = should_stop;
    setState(ConnectionState::Disconnecting);
    if (operation_generation != operation_generation_)
    {
        return;
    }
    setPresentation(QStringLiteral("Disconnecting"));
    if (operation_generation == operation_generation_ && should_stop && stop_completion_pending_ &&
        state_ == ConnectionState::Disconnecting)
    {
        engine_.stop();
    }
}

bool DashboardConnectionController::hasUsableDocument() const
{
    return document_.has_value() && !document_->cards.empty();
}

void DashboardConnectionController::prepare(std::optional<connection::AdapterSelection> selection,
                                            std::uint64_t operation_generation)
{
    if (!hasUsableDocument() || operation_generation != operation_generation_ || state_ != ConnectionState::Connecting)
    {
        return;
    }
    auto outcome = preparation_.prepare_run(*document_, std::move(selection));
    if (operation_generation != operation_generation_ || state_ != ConnectionState::Connecting)
    {
        return;
    }
    handlePreparation(std::move(outcome), operation_generation);
}

void DashboardConnectionController::handlePreparation(connection::ConnectionPreparationOutcome outcome,
                                                      std::uint64_t operation_generation)
{
    if (operation_generation != operation_generation_ || state_ != ConnectionState::Connecting)
    {
        return;
    }
    std::visit(
        [this, operation_generation](auto&& result)
        {
            using ResultType = std::decay_t<decltype(result)>;
            if constexpr (std::is_same_v<ResultType, connection::AdapterSelectionRequired>)
            {
                candidates_.replace(std::move(result.snapshot));
                setPresentation(QStringLiteral("Select an adapter"));
                if (operation_generation != operation_generation_ || state_ != ConnectionState::Connecting)
                {
                    return;
                }
                setState(ConnectionState::AdapterSelectionRequired);
            }
            else if constexpr (std::is_same_v<ResultType, connection::PreparedConnection>)
            {
                setPresentation(QStringLiteral("Connecting"), {}, QString::fromStdString(result.selected.label));
                if (operation_generation != operation_generation_ || state_ != ConnectionState::Connecting)
                {
                    return;
                }
                const std::uint64_t run_generation = ++next_run_generation_;
                active_run_generation_ = run_generation;
                engine_started_ = true;
                Status started = engine_.start(std::move(result.run));
                if (operation_generation != operation_generation_ || active_run_generation_ != run_generation ||
                    !engine_started_)
                {
                    return;
                }
                if (!started)
                {
                    active_run_generation_ = 0;
                    engine_started_ = false;
                    fail(QStringLiteral("Unable to start logging"), QString::fromStdString(started.error().detail));
                    return;
                }
            }
            else
            {
                fail(QStringLiteral("Unable to prepare logging"), QString::fromStdString(result.detail));
            }
        },
        std::move(outcome));
}

void DashboardConnectionController::handleStatus(desktop::logging::LoggingStatus status)
{
    if (destroying_ || !engine_started_ || active_run_generation_ == 0 ||
        (state_ != ConnectionState::Connecting && state_ != ConnectionState::Running &&
         state_ != ConnectionState::CarNotResponding))
    {
        return;
    }
    const std::uint64_t operation_generation = operation_generation_;
    switch (status)
    {
    case desktop::logging::LoggingStatus::Running:
        setPresentation(QStringLiteral("Connected"), {}, selected_adapter_label_);
        if (operation_generation != operation_generation_ || !engine_started_ || active_run_generation_ == 0 ||
            (state_ != ConnectionState::Connecting && state_ != ConnectionState::Running &&
             state_ != ConnectionState::CarNotResponding))
        {
            return;
        }
        setState(ConnectionState::Running);
        return;
    case desktop::logging::LoggingStatus::CarNotResponding:
        setPresentation(QStringLiteral("Car not responding"), {}, selected_adapter_label_);
        if (operation_generation != operation_generation_ || !engine_started_ || active_run_generation_ == 0 ||
            (state_ != ConnectionState::Connecting && state_ != ConnectionState::Running &&
             state_ != ConnectionState::CarNotResponding))
        {
            return;
        }
        setState(ConnectionState::CarNotResponding);
        return;
    }
}

void DashboardConnectionController::handleCompletion(desktop::logging::SessionEndReason reason, const QString& detail)
{
    if (destroying_ || (active_run_generation_ == 0 && !stop_completion_pending_))
    {
        return;
    }
    const std::uint64_t operation_generation = ++operation_generation_;
    active_run_generation_ = 0;
    engine_started_ = false;
    stop_completion_pending_ = false;
    switch (reason)
    {
    case desktop::logging::SessionEndReason::StoppedByUser:
        setPresentation(QStringLiteral("Disconnected"), detail);
        if (operation_generation != operation_generation_)
        {
            return;
        }
        setState(ConnectionState::Disconnected);
        return;
    case desktop::logging::SessionEndReason::HandshakeFailed:
        fail(QStringLiteral("Unable to start CDBG logging"), detail);
        return;
    case desktop::logging::SessionEndReason::AdapterDisconnected:
        fail(QStringLiteral("Adapter disconnected"), detail);
        return;
    case desktop::logging::SessionEndReason::RuntimeFailed:
        fail(QStringLiteral("Logging stopped unexpectedly"), detail);
        return;
    }
}

void DashboardConnectionController::setState(ConnectionState state)
{
    if (state_ == state)
    {
        return;
    }
    state_ = state;
    emit stateChanged();
}

void DashboardConnectionController::setPresentation(QString status, QString detail, QString selected_adapter)
{
    if (status_text_ == status && technical_detail_ == detail && selected_adapter_label_ == selected_adapter)
    {
        return;
    }
    status_text_ = std::move(status);
    technical_detail_ = std::move(detail);
    selected_adapter_label_ = std::move(selected_adapter);
    emit presentationChanged();
}

void DashboardConnectionController::fail(QString status, QString detail)
{
    const std::uint64_t operation_generation = operation_generation_;
    setPresentation(std::move(status), std::move(detail), selected_adapter_label_);
    if (operation_generation != operation_generation_)
    {
        return;
    }
    setState(ConnectionState::Failed);
}

} // namespace fastecu::desktop_quick
