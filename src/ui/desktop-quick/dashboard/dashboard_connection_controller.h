#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>
#include <optional>

#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/connection/desktop_connection_service.h"
#include "src/platform/desktop/common/logging/logging_engine.h"

namespace fastecu::desktop_quick
{

namespace connection = desktop::connection;

class IConnectionPreparationService
{
  public:
    virtual ~IConnectionPreparationService() = default;
    virtual connection::ConnectionPreparationOutcome
    prepare_run(const dashboard::DashboardDocument& document,
                std::optional<connection::AdapterSelection> selection) = 0;
    virtual Result<connection::AdapterDiscoverySnapshot> refresh() = 0;
};

// This is deliberately the only QObject bridge between the controller and the
// logging runtime. It relays presentation events without exposing the engine
// or any hardware object to QML.
class ILoggingEngine : public QObject
{
    Q_OBJECT

  public:
    explicit ILoggingEngine(QObject *parent = nullptr) : QObject(parent)
    {
    }
    ~ILoggingEngine() override = default;

    virtual Status start(desktop::logging::LoggingRun run) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

  signals:
    void valuesUpdated(QVector<fastecu::logging::LogSample> samples);
    void statusChanged(desktop::logging::LoggingStatus status);
    void sessionEnded(desktop::logging::SessionEndReason reason, QString detail);
};

namespace detail
{

std::unique_ptr<ILoggingEngine> make_logging_engine_bridge(desktop::logging::LoggingEngine& engine);

} // namespace detail

class AdapterCandidateModel final : public QAbstractListModel
{
    Q_OBJECT

  public:
    enum Role
    {
        CandidateIdRole = Qt::UserRole + 1,
        LabelRole,
        KindLabelRole,
    };
    Q_ENUM(Role)

    explicit AdapterCandidateModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replace(connection::AdapterDiscoverySnapshot snapshot);
    bool contains(const QString& candidate_id) const;
    std::uint64_t generation() const;

  private:
    connection::AdapterDiscoverySnapshot snapshot_{.generation = 0, .candidates = {}, .diagnostics = {}};
};

class DashboardConnectionController final : public QObject
{
    Q_OBJECT

  public:
    enum class ConnectionState
    {
        Disconnected,
        Connecting,
        AdapterSelectionRequired,
        Running,
        CarNotResponding,
        Disconnecting,
        Failed,
    };
    Q_ENUM(ConnectionState)

    Q_PROPERTY(ConnectionState state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY presentationChanged)
    Q_PROPERTY(QString technicalDetail READ technicalDetail NOTIFY presentationChanged)
    Q_PROPERTY(QString selectedAdapterLabel READ selectedAdapterLabel NOTIFY presentationChanged)
    Q_PROPERTY(QAbstractItemModel *candidates READ candidates CONSTANT)
    Q_PROPERTY(qulonglong discoveryGeneration READ discoveryGeneration NOTIFY presentationChanged)
    Q_PROPERTY(bool canConnect READ canConnect NOTIFY stateChanged)
    Q_PROPERTY(bool canDisconnect READ canDisconnect NOTIFY stateChanged)
    Q_PROPERTY(bool needsAdapterSelection READ needsAdapterSelection NOTIFY stateChanged)
    Q_PROPERTY(bool isTransitioning READ isTransitioning NOTIFY stateChanged)

    DashboardConnectionController(IConnectionPreparationService& preparation, ILoggingEngine& engine,
                                  QObject *parent = nullptr);
    DashboardConnectionController(connection::DesktopConnectionService& preparation,
                                  desktop::logging::LoggingEngine& engine, QObject *parent = nullptr);
    ~DashboardConnectionController() override;

    ConnectionState state() const;
    QString statusText() const;
    QString technicalDetail() const;
    QString selectedAdapterLabel() const;
    QAbstractItemModel *candidates();
    qulonglong discoveryGeneration() const;
    bool canConnect() const;
    bool canDisconnect() const;
    bool needsAdapterSelection() const;
    bool isTransitioning() const;

    Q_INVOKABLE void connectDashboard();
    Q_INVOKABLE void connectWithAdapter(const QString& candidate_id);
    Q_INVOKABLE void refreshAdapters();
    Q_INVOKABLE void disconnectDashboard();
    void setDocument(std::optional<dashboard::DashboardDocument> document);

  signals:
    void stateChanged();
    void presentationChanged();

  private:
    bool hasUsableDocument() const;
    void prepare(std::optional<connection::AdapterSelection> selection, std::uint64_t operation_generation);
    void handlePreparation(connection::ConnectionPreparationOutcome outcome, std::uint64_t operation_generation);
    void handleStatus(desktop::logging::LoggingStatus status);
    void handleCompletion(desktop::logging::SessionEndReason reason, const QString& detail);
    void setState(ConnectionState state);
    void setPresentation(QString status, QString detail = {}, QString selected_adapter = {});
    void fail(QString status, QString detail);

    std::unique_ptr<IConnectionPreparationService> owned_preparation_;
    std::unique_ptr<ILoggingEngine> owned_engine_;
    IConnectionPreparationService& preparation_;
    ILoggingEngine& engine_;
    AdapterCandidateModel candidates_;
    std::optional<dashboard::DashboardDocument> document_;
    ConnectionState state_ = ConnectionState::Disconnected;
    QString status_text_ = QStringLiteral("Disconnected");
    QString technical_detail_;
    QString selected_adapter_label_;
    std::uint64_t operation_generation_ = 0;
    std::uint64_t next_run_generation_ = 0;
    std::uint64_t active_run_generation_ = 0;
    bool engine_started_ = false;
    bool stop_completion_pending_ = false;
    bool destroying_ = false;
};

using ConnectionState = DashboardConnectionController::ConnectionState;

} // namespace fastecu::desktop_quick

Q_DECLARE_METATYPE(fastecu::desktop_quick::ConnectionState)
