#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <cstdint>

#include <QObject>
#include <QString>
#include <QVector>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/logging/legacy_logging_protocol_factory.h"
#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"

namespace fastecu::desktop::logging
{

struct LegacyLoggingStartRequest
{
    fastecu::logging::LoggingProtocolId protocol;
    QString protocol_filter;
    fastecu::logging::LoggingPolicy policy;
};

class LegacyLoggingCoordinatorTestAccess;

class LegacyLoggingCoordinator final : public QObject
{
    Q_OBJECT

  public:
    LegacyLoggingCoordinator(LoggingEngine& engine, LegacyLoggingProtocolFactory& factory,
                             FileActions::LogValuesStructure& values, QObject *parent = nullptr);

    fastecu::Status start(const LegacyLoggingStartRequest& request);
    void stop();
    bool hasRetainedMapping() const;
    QString activeProtocolFilter() const;

  signals:
    void valuesApplied();
    void sessionEnded(SessionEndReason reason, QString detail);
    void diagnostic(int level, QString message);

  private:
    using PreparedSessionResult = fastecu::Result<PreparedLegacyLoggingSession>;
    using ProtocolResult = fastecu::Result<std::unique_ptr<fastecu::logging::LoggingProtocol>>;

    struct ConstructionDependencies
    {
        std::function<PreparedSessionResult(const FileActions::LogValuesStructure&, fastecu::logging::LoggingProtocolId,
                                            const QString&, fastecu::logging::LoggingPolicy)>
            prepare_session;
        std::function<ProtocolResult(const LegacyProtocolRequest&)> create_protocol;
        std::function<fastecu::Status(LoggingRun)> start_engine;
        std::function<void()> stop_engine;
    };

    struct TestingTag
    {
    };

    LegacyLoggingCoordinator(LoggingEngine& engine, FileActions::LogValuesStructure& values,
                             ConstructionDependencies dependencies, TestingTag, QObject *parent = nullptr);

    void handleSamples(std::uint64_t generation, QVector<fastecu::logging::LogSample> samples);
    void handleSessionEnded(SessionEndReason reason, QString detail);
    void clearActiveMapping();

    LoggingEngine& engine_;
    FileActions::LogValuesStructure& values_;
    ConstructionDependencies dependencies_;
    std::optional<LegacyLoggingMapping> active_mapping_;
    QString active_protocol_filter_;
    QMetaObject::Connection sample_connection_;
    std::uint64_t active_run_generation_ = 0;
    std::uint64_t next_run_generation_ = 0;

    friend class LegacyLoggingCoordinatorTestAccess;
};

} // namespace fastecu::desktop::logging
