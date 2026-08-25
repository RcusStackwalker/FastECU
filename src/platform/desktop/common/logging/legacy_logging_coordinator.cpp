#include "src/platform/desktop/common/logging/legacy_logging_coordinator.h"

#include <exception>
#include <utility>

#include "src/backend/ports/event_sink.h"
#include "src/platform/desktop/common/logging/logging_value_adapter.h"

namespace fastecu::desktop::logging
{

LegacyLoggingCoordinator::LegacyLoggingCoordinator(LoggingEngine& engine, LegacyLoggingProtocolFactory& factory,
                                                   FileActions::LogValuesStructure& values, QObject *parent)
    : LegacyLoggingCoordinator(
          engine, values,
          {
              .prepare_session = make_prepared_legacy_logging_session,
              .create_protocol = [&factory](const LegacyProtocolRequest& request) { return factory.create(request); },
              .start_engine = [&engine](LoggingRun run) { return engine.start(std::move(run)); },
              .stop_engine = [&engine]() { engine.stop(); },
          },
          TestingTag{}, parent)
{
}

LegacyLoggingCoordinator::LegacyLoggingCoordinator(LoggingEngine& engine, FileActions::LogValuesStructure& values,
                                                   ConstructionDependencies dependencies, TestingTag, QObject *parent)
    : QObject(parent), engine_(engine), values_(values), dependencies_(std::move(dependencies))
{
    connect(&engine_, &LoggingEngine::sessionEnded, this, &LegacyLoggingCoordinator::handleSessionEnded);
    connect(&engine_, &LoggingEngine::LOG_E, this, [this](const QString& message, bool, bool)
            { emit diagnostic(static_cast<int>(fastecu::LogLevel::Error), message); });
    connect(&engine_, &LoggingEngine::LOG_W, this, [this](const QString& message, bool, bool)
            { emit diagnostic(static_cast<int>(fastecu::LogLevel::Warning), message); });
    connect(&engine_, &LoggingEngine::LOG_I, this, [this](const QString& message, bool, bool)
            { emit diagnostic(static_cast<int>(fastecu::LogLevel::Info), message); });
    connect(&engine_, &LoggingEngine::LOG_D, this, [this](const QString& message, bool, bool)
            { emit diagnostic(static_cast<int>(fastecu::LogLevel::Debug), message); });
}

fastecu::Status LegacyLoggingCoordinator::start(const LegacyLoggingStartRequest& request)
{
    if (active_mapping_)
    {
        const fastecu::Error error{fastecu::ErrorKind::InvalidConfig, "a legacy logging run is already active"};
        reportStartError(error);
        return std::unexpected(error);
    }

    auto prepared = dependencies_.prepare_session(values_, request.protocol, request.protocol_filter, request.policy);
    if (!prepared.has_value())
    {
        const fastecu::Error error = prepared.error();
        reportStartError(error);
        return std::unexpected(error);
    }

    active_mapping_.emplace(std::move(prepared->mapping));
    active_protocol_filter_ = request.protocol_filter;
    active_run_generation_ = ++next_run_generation_;
    const LegacyProtocolRequest protocol_request{
        .protocol = request.protocol,
        .channels = prepared->session.channels(),
        .ssm_response_offsets = active_mapping_->response_offsets,
    };

    ProtocolResult protocol = fastecu::fail(fastecu::ErrorKind::Internal, "legacy protocol factory did not return");
    try
    {
        protocol = dependencies_.create_protocol(protocol_request);
    }
    catch (const std::exception& error)
    {
        clearActiveMapping();
        const fastecu::Error failure{fastecu::ErrorKind::Internal, error.what()};
        reportStartError(failure);
        return std::unexpected(failure);
    }
    catch (...)
    {
        clearActiveMapping();
        const fastecu::Error error{fastecu::ErrorKind::Internal, "legacy protocol factory threw an unknown exception"};
        reportStartError(error);
        return std::unexpected(error);
    }
    if (!protocol.has_value())
    {
        const fastecu::Error error = protocol.error();
        clearActiveMapping();
        reportStartError(error);
        return std::unexpected(error);
    }
    if (!*protocol)
    {
        clearActiveMapping();
        const fastecu::Error error{fastecu::ErrorKind::Internal, "legacy protocol factory returned null"};
        reportStartError(error);
        return std::unexpected(error);
    }

    const std::uint64_t run_generation = active_run_generation_;
    sample_connection_ = connect(&engine_, &LoggingEngine::valuesUpdated, this,
                                 [this, run_generation](QVector<fastecu::logging::LogSample> samples)
                                 { handleSamples(run_generation, std::move(samples)); });

    fastecu::Status started = fastecu::fail(fastecu::ErrorKind::Internal, "logging engine did not return");
    try
    {
        started =
            dependencies_.start_engine({.session = std::move(prepared->session), .protocol = std::move(*protocol)});
    }
    catch (const std::exception& error)
    {
        clearActiveMapping();
        const fastecu::Error failure{fastecu::ErrorKind::Internal, error.what()};
        reportStartError(failure);
        return std::unexpected(failure);
    }
    catch (...)
    {
        clearActiveMapping();
        const fastecu::Error error{fastecu::ErrorKind::Internal, "logging engine threw an unknown exception"};
        reportStartError(error);
        return std::unexpected(error);
    }
    if (!started.has_value())
    {
        const fastecu::Error error = started.error();
        clearActiveMapping();
        return std::unexpected(error);
    }
    return {};
}

void LegacyLoggingCoordinator::stop()
{
    if (!active_mapping_)
    {
        return;
    }
    clearActiveMapping();
    dependencies_.stop_engine();
}

bool LegacyLoggingCoordinator::hasRetainedMapping() const
{
    return active_mapping_.has_value();
}

QString LegacyLoggingCoordinator::activeProtocolFilter() const
{
    return active_protocol_filter_;
}

void LegacyLoggingCoordinator::handleSamples(std::uint64_t generation, QVector<fastecu::logging::LogSample> samples)
{
    if (!active_mapping_ || active_run_generation_ != generation)
    {
        return;
    }

    for (const auto& sample : samples)
    {
        const auto applied = apply_log_sample(*active_mapping_, sample, values_);
        if (!applied.has_value())
        {
            emit diagnostic(static_cast<int>(fastecu::LogLevel::Error), QString::fromStdString(applied.error().detail));
        }
    }
    emit valuesApplied();
}

void LegacyLoggingCoordinator::handleSessionEnded(SessionEndReason reason, QString detail)
{
    clearActiveMapping();
    emit sessionEnded(reason, std::move(detail));
}

void LegacyLoggingCoordinator::reportStartError(const fastecu::Error& error)
{
    emit diagnostic(static_cast<int>(fastecu::LogLevel::Error),
                    QStringLiteral("Logging session failed to start: ") + QString::fromStdString(error.detail));
}

void LegacyLoggingCoordinator::clearActiveMapping()
{
    disconnect(sample_connection_);
    sample_connection_ = {};
    active_run_generation_ = 0;
    active_mapping_.reset();
    active_protocol_filter_.clear();
}

} // namespace fastecu::desktop::logging
