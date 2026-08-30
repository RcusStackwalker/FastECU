#include <QtTest>
#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>

#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "src/platform/desktop/common/logging/legacy_logging_coordinator.h"

namespace fastecu::desktop::logging
{
namespace
{

using fastecu::logging::LoggingChannel;
using fastecu::logging::LoggingPolicy;
using fastecu::logging::LoggingProtocol;
using fastecu::logging::LoggingProtocolId;
using fastecu::logging::LogSample;
using fastecu::logging::PollData;

constexpr LoggingPolicy kPolicy{
    .poll_timeout_ms = 50,
    .car_silence_miss_threshold = 20,
    .reconnect_initial_delay_ms = 4000,
    .reconnect_period_ms = 1000,
    .max_reconnect_attempts = std::nullopt,
};

class StubProtocol : public LoggingProtocol
{
  public:
    fastecu::Status start(const fastecu::ICancellationToken&) override
    {
        return {};
    }

    fastecu::Result<PollData> poll(int, const fastecu::ICancellationToken&) override
    {
        return PollData{};
    }

    fastecu::Status stop() override
    {
        return {};
    }
};

class DestructionCountingProtocol final : public StubProtocol
{
  public:
    explicit DestructionCountingProtocol(int& destructions) : destructions_(destructions)
    {
    }
    ~DestructionCountingProtocol() override
    {
        ++destructions_;
    }

  private:
    int& destructions_;
};

void append_value(FileActions::LogValuesStructure& values, const QString& id, const QString& protocol,
                  const QString& enabled, const QString& format = QStringLiteral("0.00"))
{
    values.log_value_id.append(id);
    values.log_value_protocol.append(protocol);
    values.log_value_name.append(id);
    values.log_value_description.append(id);
    values.log_value_ecu_byte_index.append(QStringLiteral("0"));
    values.log_value_ecu_bit.append(QStringLiteral("0"));
    values.log_value_target.append(QStringLiteral("ECU"));
    values.log_value_address.append(QStringLiteral("000010"));
    values.log_value_conversions.append({{"rpm", "x", format.toStdString(), "0", "100", "1"}});
    values.log_value_length.append(QStringLiteral("1"));
    values.log_value.append(QStringLiteral("unchanged-") + id);
    values.log_value_enabled.append(enabled);
}

FileActions::LogValuesStructure values_for(const QString& protocol = QStringLiteral("SSM"))
{
    FileActions::LogValuesStructure values;
    append_value(values, QStringLiteral("coolant"), protocol, QStringLiteral("1"));
    append_value(values, QStringLiteral("rpm"), protocol, QStringLiteral("1"));
    values.lower_panel_log_value_id = {QStringLiteral("rpm"), QStringLiteral("coolant")};
    return values;
}

LegacyLoggingStartRequest request_for(LoggingProtocolId protocol, QString filter)
{
    return {.protocol = protocol, .protocol_filter = std::move(filter), .policy = kPolicy};
}

std::unique_ptr<LoggingProtocol> protocol()
{
    return std::make_unique<StubProtocol>();
}

void expect_single_start_diagnostic(const fastecu::Status& status, const QSignalSpy& diagnostic_spy,
                                    const QSignalSpy& ended_spy)
{
    QVERIFY(!status.has_value());
    QCOMPARE(diagnostic_spy.count(), 1);
    QCOMPARE(diagnostic_spy.at(0).at(0).toInt(), static_cast<int>(fastecu::LogLevel::Error));
    QCOMPARE(diagnostic_spy.at(0).at(1).toString(),
             QStringLiteral("Logging session failed to start: ") + QString::fromStdString(status.error().detail));
    QCOMPARE(ended_spy.count(), 0);
}

} // namespace

class LegacyLoggingCoordinatorTestAccess
{
  public:
    using Dependencies = LegacyLoggingCoordinator::ConstructionDependencies;

    static std::unique_ptr<LegacyLoggingCoordinator>
    make(LoggingEngine& engine, FileActions::LogValuesStructure& values, Dependencies dependencies)
    {
        return std::unique_ptr<LegacyLoggingCoordinator>(new LegacyLoggingCoordinator(
            engine, values, std::move(dependencies), LegacyLoggingCoordinator::TestingTag{}));
    }
};

namespace
{

LegacyLoggingCoordinatorTestAccess::Dependencies successful_dependencies()
{
    return {
        .prepare_session = make_prepared_legacy_logging_session,
        .create_protocol = [](const LegacyProtocolRequest&)
        { return fastecu::Result<std::unique_ptr<LoggingProtocol>>(protocol()); },
        .start_engine = [](LoggingRun) { return fastecu::Status{}; },
        .stop_engine = []() {},
    };
}

} // namespace

class LegacyLoggingCoordinatorTest : public QObject
{
    Q_OBJECT

  private slots:
    void start_passes_selected_channels_policy_filter_and_ssm_offsets();
    void snapshot_validation_failure_does_not_create_protocol_or_retain_mapping();
    void factory_failures_do_not_start_engine_or_retain_mapping();
    void engine_rejection_destroys_protocol_and_clears_mapping();
    void second_start_is_rejected_without_replacing_the_retained_mapping();
    void engine_invocation_exceptions_report_one_diagnostic_without_terminal();
    void successful_samples_apply_stable_ids_after_reorder_and_emit_one_cue();
    void valid_samples_are_applied_before_immediately_following_terminal_delivery();
    void unknown_sample_id_reports_error_without_stopping_later_batches();
    void terminal_forwarding_clears_mapping_before_observers_run();
    void stop_and_all_terminal_reasons_clear_the_retained_mapping();
    void stop_without_an_owned_run_does_not_stop_the_engine();
    void stale_queued_samples_from_a_stopped_run_do_not_apply_after_restart();
    void diagnostics_preserve_engine_severity();
};

void LegacyLoggingCoordinatorTest::start_passes_selected_channels_policy_filter_and_ssm_offsets()
{
    struct Recording
    {
        LegacyProtocolRequest request;
        LoggingPolicy policy{};
        int starts = 0;
    } recording;

    const auto exercise =
        [&recording](LoggingProtocolId protocol_id, const QString& filter, const QString& row_protocol)
    {
        LoggingEngine engine;
        auto values = values_for(row_protocol);
        auto dependencies = successful_dependencies();
        dependencies.create_protocol = [&recording](const LegacyProtocolRequest& request)
        {
            recording.request = request;
            return fastecu::Result<std::unique_ptr<LoggingProtocol>>(protocol());
        };
        dependencies.start_engine = [&recording](LoggingRun run)
        {
            ++recording.starts;
            recording.policy = run.session.policy();
            return fastecu::Status{};
        };
        auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, std::move(dependencies));
        const int starts_before = recording.starts;

        QVERIFY(coordinator->start(request_for(protocol_id, filter)).has_value());
        QCOMPARE(recording.starts, starts_before + 1);
        QCOMPARE(recording.request.protocol, protocol_id);
        QCOMPARE(recording.request.channels.size(), std::size_t{2});
        QCOMPARE(recording.request.channels.at(0).id, std::string("rpm"));
        QCOMPARE(recording.request.channels.at(1).id, std::string("coolant"));
        QCOMPARE(recording.policy.poll_timeout_ms, kPolicy.poll_timeout_ms);
        QCOMPARE(coordinator->activeProtocolFilter(), filter);
        coordinator->stop();
        QVERIFY(!coordinator->hasRetainedMapping());
        QVERIFY(coordinator->activeProtocolFilter().isEmpty());
    };

    exercise(LoggingProtocolId::MutDma, QStringLiteral("MUT_DMA"), QStringLiteral("MUT_DMA"));
    QCOMPARE(recording.request.ssm_response_offsets.size(), std::size_t{0});
    exercise(LoggingProtocolId::Cdbg, QStringLiteral("CDBG"), QStringLiteral("CDBG"));
    QCOMPARE(recording.request.ssm_response_offsets.size(), std::size_t{0});
    exercise(LoggingProtocolId::Ssm, QStringLiteral("SSM"), QStringLiteral("SSM"));
    QCOMPARE(recording.request.ssm_response_offsets, std::vector<std::size_t>({0, 1}));
}

void LegacyLoggingCoordinatorTest::snapshot_validation_failure_does_not_create_protocol_or_retain_mapping()
{
    LoggingEngine engine;
    auto values = values_for();
    values.log_value_length.clear();
    auto dependencies = successful_dependencies();
    int create_calls = 0;
    int start_calls = 0;
    dependencies.create_protocol = [&create_calls](const LegacyProtocolRequest&)
    {
        ++create_calls;
        return fastecu::Result<std::unique_ptr<LoggingProtocol>>(protocol());
    };
    dependencies.start_engine = [&start_calls](LoggingRun)
    {
        ++start_calls;
        return fastecu::Status{};
    };
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, std::move(dependencies));
    QSignalSpy diagnostic_spy(coordinator.get(), &LegacyLoggingCoordinator::diagnostic);
    QSignalSpy ended_spy(coordinator.get(), &LegacyLoggingCoordinator::sessionEnded);

    const auto status = coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM")));

    expect_single_start_diagnostic(status, diagnostic_spy, ended_spy);
    QCOMPARE(create_calls, 0);
    QCOMPARE(start_calls, 0);
    QVERIFY(!coordinator->hasRetainedMapping());
}

void LegacyLoggingCoordinatorTest::factory_failures_do_not_start_engine_or_retain_mapping()
{
    const auto exercise = [](auto create_protocol)
    {
        LoggingEngine engine;
        auto values = values_for();
        auto dependencies = successful_dependencies();
        int start_calls = 0;
        dependencies.create_protocol = std::move(create_protocol);
        dependencies.start_engine = [&start_calls](LoggingRun)
        {
            ++start_calls;
            return fastecu::Status{};
        };
        auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, std::move(dependencies));
        QSignalSpy diagnostic_spy(coordinator.get(), &LegacyLoggingCoordinator::diagnostic);
        QSignalSpy ended_spy(coordinator.get(), &LegacyLoggingCoordinator::sessionEnded);

        const auto status = coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM")));

        expect_single_start_diagnostic(status, diagnostic_spy, ended_spy);
        QCOMPARE(start_calls, 0);
        QVERIFY(!coordinator->hasRetainedMapping());
    };

    exercise([](const LegacyProtocolRequest&) -> fastecu::Result<std::unique_ptr<LoggingProtocol>>
             { return fastecu::fail(fastecu::ErrorKind::Disconnected, "factory failed"); });
    exercise([](const LegacyProtocolRequest&)
             { return fastecu::Result<std::unique_ptr<LoggingProtocol>>(std::unique_ptr<LoggingProtocol>{}); });
    exercise([](const LegacyProtocolRequest&) -> fastecu::Result<std::unique_ptr<LoggingProtocol>>
             { throw std::runtime_error("factory threw"); });
}

void LegacyLoggingCoordinatorTest::engine_rejection_destroys_protocol_and_clears_mapping()
{
    LoggingEngine engine;
    auto values = values_for();
    int destructions = 0;
    const fastecu::Error rejected{fastecu::ErrorKind::InvalidConfig, "engine rejected the run"};
    auto dependencies = successful_dependencies();
    dependencies.create_protocol = [&destructions](const LegacyProtocolRequest&)
    {
        return fastecu::Result<std::unique_ptr<LoggingProtocol>>(
            std::make_unique<DestructionCountingProtocol>(destructions));
    };
    dependencies.start_engine = [&engine, &rejected](LoggingRun)
    {
        emit engine.LOG_E(QStringLiteral("Logging session failed to start: ") + QString::fromStdString(rejected.detail),
                          true, true);
        return fastecu::Status(std::unexpected(rejected));
    };
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, std::move(dependencies));
    QSignalSpy diagnostic_spy(coordinator.get(), &LegacyLoggingCoordinator::diagnostic);
    QSignalSpy ended_spy(coordinator.get(), &LegacyLoggingCoordinator::sessionEnded);

    const auto status = coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM")));

    expect_single_start_diagnostic(status, diagnostic_spy, ended_spy);
    QCOMPARE(QString::fromStdString(status.error().detail), QString::fromStdString(rejected.detail));
    QCOMPARE(destructions, 1);
    QVERIFY(!coordinator->hasRetainedMapping());
}

void LegacyLoggingCoordinatorTest::second_start_is_rejected_without_replacing_the_retained_mapping()
{
    LoggingEngine engine;
    auto values = values_for();
    int create_calls = 0;
    int start_calls = 0;
    auto dependencies = successful_dependencies();
    dependencies.create_protocol = [&create_calls](const LegacyProtocolRequest&)
    {
        ++create_calls;
        return fastecu::Result<std::unique_ptr<LoggingProtocol>>(protocol());
    };
    dependencies.start_engine = [&start_calls](LoggingRun)
    {
        ++start_calls;
        return fastecu::Status{};
    };
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, std::move(dependencies));
    QSignalSpy diagnostic_spy(coordinator.get(), &LegacyLoggingCoordinator::diagnostic);
    QSignalSpy ended_spy(coordinator.get(), &LegacyLoggingCoordinator::sessionEnded);

    QVERIFY(coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM"))).has_value());
    const auto status = coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("OTHER")));

    expect_single_start_diagnostic(status, diagnostic_spy, ended_spy);
    QCOMPARE(create_calls, 1);
    QCOMPARE(start_calls, 1);
    QVERIFY(coordinator->hasRetainedMapping());
    QCOMPARE(coordinator->activeProtocolFilter(), QStringLiteral("SSM"));
}

void LegacyLoggingCoordinatorTest::engine_invocation_exceptions_report_one_diagnostic_without_terminal()
{
    const auto exercise = [](auto start_engine)
    {
        LoggingEngine engine;
        auto values = values_for();
        auto dependencies = successful_dependencies();
        dependencies.start_engine = std::move(start_engine);
        auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, std::move(dependencies));
        QSignalSpy diagnostic_spy(coordinator.get(), &LegacyLoggingCoordinator::diagnostic);
        QSignalSpy ended_spy(coordinator.get(), &LegacyLoggingCoordinator::sessionEnded);

        const auto status = coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM")));

        expect_single_start_diagnostic(status, diagnostic_spy, ended_spy);
        QVERIFY(!coordinator->hasRetainedMapping());
    };

    exercise([](LoggingRun) -> fastecu::Status { throw std::runtime_error("engine threw"); });
    exercise([](LoggingRun) -> fastecu::Status { throw 7; });
}

void LegacyLoggingCoordinatorTest::successful_samples_apply_stable_ids_after_reorder_and_emit_one_cue()
{
    LoggingEngine engine;
    auto values = values_for();
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, successful_dependencies());
    QSignalSpy applied_spy(coordinator.get(), &LegacyLoggingCoordinator::valuesApplied);

    QVERIFY(coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM"))).has_value());
    QVERIFY(coordinator->hasRetainedMapping());
    QCOMPARE(coordinator->activeProtocolFilter(), QStringLiteral("SSM"));
    emit engine.valuesUpdated({LogSample{.channel_id = "rpm", .numeric_value = 1234.5},
                               LogSample{.channel_id = "coolant", .numeric_value = 88.0}});

    QTRY_COMPARE(applied_spy.count(), 1);
    QCOMPARE(values.log_value.at(1), QStringLiteral("1234.50"));
    QCOMPARE(values.log_value.at(0), QStringLiteral("88.00"));
}

void LegacyLoggingCoordinatorTest::valid_samples_are_applied_before_immediately_following_terminal_delivery()
{
    LoggingEngine engine;
    auto values = values_for();
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, successful_dependencies());
    QStringList observed_events;
    connect(coordinator.get(), &LegacyLoggingCoordinator::valuesApplied, this,
            [&observed_events]() { observed_events.append(QStringLiteral("valuesApplied")); });
    connect(coordinator.get(), &LegacyLoggingCoordinator::sessionEnded, this,
            [&observed_events](SessionEndReason, const QString&)
            { observed_events.append(QStringLiteral("sessionEnded")); });

    QVERIFY(coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM"))).has_value());
    emit engine.valuesUpdated({LogSample{.channel_id = "rpm", .numeric_value = 3210.0}});
    emit engine.sessionEnded(SessionEndReason::RuntimeFailed, QStringLiteral("poll failed"));

    QCoreApplication::sendPostedEvents(coordinator.get(), QEvent::MetaCall);
    QCOMPARE(observed_events, QStringList({QStringLiteral("valuesApplied"), QStringLiteral("sessionEnded")}));
    QCOMPARE(values.log_value.at(1), QStringLiteral("3210.00"));
}

void LegacyLoggingCoordinatorTest::unknown_sample_id_reports_error_without_stopping_later_batches()
{
    LoggingEngine engine;
    auto values = values_for();
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, successful_dependencies());
    QSignalSpy diagnostic_spy(coordinator.get(), &LegacyLoggingCoordinator::diagnostic);
    QSignalSpy applied_spy(coordinator.get(), &LegacyLoggingCoordinator::valuesApplied);

    QVERIFY(coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM"))).has_value());
    emit engine.valuesUpdated({LogSample{.channel_id = "unknown", .numeric_value = 7.0}});
    QTRY_COMPARE(diagnostic_spy.count(), 1);
    QCOMPARE(diagnostic_spy.at(0).at(0).toInt(), static_cast<int>(fastecu::LogLevel::Error));
    QCOMPARE(values.log_value.at(0), QStringLiteral("unchanged-coolant"));
    QCOMPARE(values.log_value.at(1), QStringLiteral("unchanged-rpm"));
    QVERIFY(coordinator->hasRetainedMapping());

    emit engine.valuesUpdated({LogSample{.channel_id = "rpm", .numeric_value = 42.0}});
    QTRY_COMPARE(applied_spy.count(), 2);
    QCOMPARE(values.log_value.at(1), QStringLiteral("42.00"));
    QVERIFY(coordinator->hasRetainedMapping());
}

void LegacyLoggingCoordinatorTest::terminal_forwarding_clears_mapping_before_observers_run()
{
    LoggingEngine engine;
    auto values = values_for();
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, successful_dependencies());
    bool mapping_was_cleared = false;
    connect(coordinator.get(), &LegacyLoggingCoordinator::sessionEnded, this,
            [&mapping_was_cleared, &coordinator](SessionEndReason, const QString&)
            { mapping_was_cleared = !coordinator->hasRetainedMapping(); });

    QVERIFY(coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM"))).has_value());
    emit engine.sessionEnded(SessionEndReason::RuntimeFailed, QStringLiteral("poll failed"));

    QVERIFY(mapping_was_cleared);
    QVERIFY(!coordinator->hasRetainedMapping());
    QVERIFY(coordinator->activeProtocolFilter().isEmpty());
}

void LegacyLoggingCoordinatorTest::stop_and_all_terminal_reasons_clear_the_retained_mapping()
{
    const auto exercise_terminal = [](SessionEndReason reason)
    {
        LoggingEngine engine;
        auto values = values_for();
        auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, successful_dependencies());
        QVERIFY(coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM"))).has_value());
        emit engine.sessionEnded(reason, QStringLiteral("ended"));
        QVERIFY(!coordinator->hasRetainedMapping());
    };

    exercise_terminal(SessionEndReason::AdapterDisconnected);
    exercise_terminal(SessionEndReason::HandshakeFailed);
    exercise_terminal(SessionEndReason::RuntimeFailed);
    exercise_terminal(SessionEndReason::StoppedByUser);

    LoggingEngine engine;
    auto values = values_for();
    int stop_calls = 0;
    auto dependencies = successful_dependencies();
    dependencies.stop_engine = [&stop_calls]() { ++stop_calls; };
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, std::move(dependencies));
    QVERIFY(coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM"))).has_value());
    coordinator->stop();
    QCOMPARE(stop_calls, 1);
    QVERIFY(!coordinator->hasRetainedMapping());
}

void LegacyLoggingCoordinatorTest::stop_without_an_owned_run_does_not_stop_the_engine()
{
    LoggingEngine engine;
    auto values = values_for();
    int stop_calls = 0;
    auto dependencies = successful_dependencies();
    dependencies.stop_engine = [&stop_calls]() { ++stop_calls; };
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, std::move(dependencies));

    coordinator->stop();

    QCOMPARE(stop_calls, 0);
    QVERIFY(!coordinator->hasRetainedMapping());
    QVERIFY(coordinator->activeProtocolFilter().isEmpty());
}

void LegacyLoggingCoordinatorTest::stale_queued_samples_from_a_stopped_run_do_not_apply_after_restart()
{
    LoggingEngine engine;
    auto values = values_for();
    int stop_calls = 0;
    auto dependencies = successful_dependencies();
    dependencies.stop_engine = [&stop_calls]() { ++stop_calls; };
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, std::move(dependencies));
    QSignalSpy applied_spy(coordinator.get(), &LegacyLoggingCoordinator::valuesApplied);

    QVERIFY(coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM"))).has_value());
    std::thread stale_worker_delivery(
        [&engine]() { emit engine.valuesUpdated({LogSample{.channel_id = "rpm", .numeric_value = 10.0}}); });
    stale_worker_delivery.join();
    coordinator->stop();
    QVERIFY(coordinator->start(request_for(LoggingProtocolId::Ssm, QStringLiteral("SSM"))).has_value());

    QCoreApplication::sendPostedEvents(coordinator.get(), QEvent::MetaCall);
    QCOMPARE(stop_calls, 1);
    QCOMPARE(applied_spy.count(), 0);
    QCOMPARE(values.log_value.at(1), QStringLiteral("unchanged-rpm"));

    emit engine.valuesUpdated({LogSample{.channel_id = "rpm", .numeric_value = 42.0}});
    QTRY_COMPARE(applied_spy.count(), 1);
    QCOMPARE(values.log_value.at(1), QStringLiteral("42.00"));
}

void LegacyLoggingCoordinatorTest::diagnostics_preserve_engine_severity()
{
    LoggingEngine engine;
    auto values = values_for();
    auto coordinator = LegacyLoggingCoordinatorTestAccess::make(engine, values, successful_dependencies());
    QSignalSpy diagnostic_spy(coordinator.get(), &LegacyLoggingCoordinator::diagnostic);

    emit engine.LOG_W(QStringLiteral("warning"), true, true);

    QCOMPARE(diagnostic_spy.count(), 1);
    QCOMPARE(diagnostic_spy.at(0).at(0).toInt(), static_cast<int>(fastecu::LogLevel::Warning));
    QCOMPARE(diagnostic_spy.at(0).at(1).toString(), QStringLiteral("warning"));
}

} // namespace fastecu::desktop::logging

QTEST_MAIN(fastecu::desktop::logging::LegacyLoggingCoordinatorTest)

#include "legacy_logging_coordinator_test.moc"
