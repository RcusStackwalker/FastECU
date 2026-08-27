#include <QSignalSpy>
#include <QtTest>

#include <memory>

#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"

namespace fastecu::desktop_quick
{
namespace
{

dashboard::DashboardDocument usable_document()
{
    return {.cards = {dashboard::DashboardCard{}}};
}

dashboard::DashboardDocument empty_document()
{
    return {};
}

class CountingProtocol final : public logging::LoggingProtocol
{
  public:
    explicit CountingProtocol(std::shared_ptr<int> destroyed) : destroyed_(std::move(destroyed))
    {
    }
    ~CountingProtocol() override
    {
        ++*destroyed_;
    }

    Status start(const ICancellationToken&) override
    {
        return {};
    }
    Result<logging::PollData> poll(int, const ICancellationToken&) override
    {
        return logging::PollData{};
    }
    Status stop() override
    {
        return {};
    }

  private:
    std::shared_ptr<int> destroyed_;
};

connection::PreparedConnection prepared_connection(std::shared_ptr<int> destroyed = std::make_shared<int>(0))
{
    auto session = logging::make_logging_session(
        logging::LoggingProtocolId::Cdbg,
        {logging::LoggingChannel{.id = "rpm",
                                 .address = 0x804cfc,
                                 .length = 2,
                                 .raw_assembly = logging::RawAssembly::UnsignedIntegerDecimal,
                                 .from_byte_expression = "x",
                                 .unit = "rpm",
                                 .decimal_precision = 0}},
        logging::LoggingPolicy{.poll_timeout_ms = 10,
                               .car_silence_miss_threshold = 1,
                               .reconnect_initial_delay_ms = 0,
                               .reconnect_period_ms = 10,
                               .max_reconnect_attempts = 1});
    Q_ASSERT(session);
    return {.run = desktop::logging::LoggingRun{std::move(*session), std::make_unique<CountingProtocol>(destroyed)},
            .selected = {.candidate_id = "socketcan:can0",
                         .kind = dashboard::AdapterKind::SocketCan,
                         .vendor = "Linux",
                         .display_name = "can0",
                         .label = "Linux CAN (can0)"}};
}

connection::AdapterDiscoverySnapshot snapshot(std::uint64_t generation,
                                              std::initializer_list<const char *> candidate_ids)
{
    connection::AdapterDiscoverySnapshot result{.generation = generation, .candidates = {}, .diagnostics = {}};
    for (const char *candidate_id : candidate_ids)
    {
        result.candidates.push_back({.candidate_id = candidate_id,
                                     .kind = dashboard::AdapterKind::SocketCan,
                                     .vendor = "Linux",
                                     .display_name = candidate_id,
                                     .label = QString::fromUtf8(candidate_id).toStdString()});
    }
    return result;
}

class FakePreparationService final : public IConnectionPreparationService
{
  public:
    connection::ConnectionPreparationOutcome prepare_run(const dashboard::DashboardDocument&,
                                                         std::optional<connection::AdapterSelection> selection) override
    {
        ++prepare_calls;
        last_selection = std::move(selection);
        if (next_preparation)
        {
            auto result = std::move(*next_preparation);
            next_preparation.reset();
            return result;
        }
        return Error{ErrorKind::Internal, "unexpected prepare"};
    }

    Result<connection::AdapterDiscoverySnapshot> refresh() override
    {
        ++refresh_calls;
        if (next_refresh)
        {
            auto result = std::move(*next_refresh);
            next_refresh.reset();
            return result;
        }
        return snapshot(1, {});
    }

    int prepare_calls = 0;
    int refresh_calls = 0;
    std::optional<connection::AdapterSelection> last_selection;
    std::optional<connection::ConnectionPreparationOutcome> next_preparation;
    std::optional<Result<connection::AdapterDiscoverySnapshot>> next_refresh;
};

class FakeLoggingEngine final : public ILoggingEngine
{
  public:
    Status start(desktop::logging::LoggingRun) override
    {
        ++start_calls;
        return start_result;
    }

    void stop() override
    {
        ++stop_calls;
        if (publish_stop_completion)
        {
            emit sessionEnded(desktop::logging::SessionEndReason::StoppedByUser, {});
        }
    }
    bool isRunning() const override
    {
        return running;
    }

    void publishStatus(desktop::logging::LoggingStatus status)
    {
        emit statusChanged(status);
    }
    void publishEnded(desktop::logging::SessionEndReason reason, QString detail = {})
    {
        emit sessionEnded(reason, detail);
    }

    Status start_result{};
    int start_calls = 0;
    int stop_calls = 0;
    bool running = false;
    bool publish_stop_completion = false;
};

class DashboardConnectionControllerTest : public QObject
{
    Q_OBJECT

  private:
    static void require_selection(DashboardConnectionController& controller, FakePreparationService& preparation,
                                  std::uint64_t generation = 1, const char *candidate_id = "old")
    {
        preparation.next_preparation = connection::AdapterSelectionRequired{
            .snapshot = snapshot(generation, {candidate_id}),
            .reason = connection::AdapterSelectionRequired::Reason::NoPreference,
        };
        controller.setDocument(usable_document());
        controller.connectDashboard();
        QCOMPARE(controller.state(), ConnectionState::AdapterSelectionRequired);
    }

    static void connect_prepared(DashboardConnectionController& controller, FakePreparationService& preparation)
    {
        preparation.next_preparation = prepared_connection();
        controller.setDocument(usable_document());
        controller.connectDashboard();
        QCOMPARE(controller.state(), ConnectionState::Connecting);
    }

  private slots:
    void documentOnlyEnablesExplicitConnect()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);

        QCOMPARE(controller.state(), ConnectionState::Disconnected);
        QVERIFY(!controller.canConnect());

        QSignalSpy state_changes(&controller, &DashboardConnectionController::stateChanged);
        controller.setDocument(empty_document());

        QVERIFY(!controller.canConnect());
        controller.connectDashboard();
        QCOMPARE(preparation.prepare_calls, 0);
        QCOMPARE(engine.start_calls, 0);

        controller.setDocument(usable_document());

        QVERIFY(controller.canConnect());
        QCOMPARE(state_changes.count(), 1);

        controller.setDocument(empty_document());

        QVERIFY(!controller.canConnect());
        QCOMPARE(state_changes.count(), 2);
        QCOMPARE(preparation.prepare_calls, 0);
        QCOMPARE(engine.start_calls, 0);
    }

    void explicitConnectPublishesConnectingBeforeSelectionIsRequired()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);
        controller.setDocument(usable_document());
        QSignalSpy states(&controller, &DashboardConnectionController::stateChanged);

        preparation.next_preparation = connection::AdapterSelectionRequired{
            .snapshot = snapshot(1, {"old"}),
            .reason = connection::AdapterSelectionRequired::Reason::NoPreference,
        };
        controller.connectDashboard();

        QCOMPARE(states.count(), 2);
        QCOMPARE(controller.state(), ConnectionState::AdapterSelectionRequired);
        QCOMPARE(controller.candidates()->rowCount(), 1);
        const QModelIndex index = controller.candidates()->index(0, 0);
        QCOMPARE(controller.candidates()->data(index, AdapterCandidateModel::CandidateIdRole).toString(), "old");
        QCOMPARE(controller.candidates()->data(index, AdapterCandidateModel::KindLabelRole).toString(), "SocketCAN");
        QVERIFY(controller.needsAdapterSelection());
    }

    void selectedAdapterUsesCurrentGeneration()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);
        require_selection(controller, preparation, 7, "socketcan:can0");
        preparation.next_preparation = Error{ErrorKind::Internal, "not opened"};

        controller.connectWithAdapter("socketcan:can0");

        QVERIFY(preparation.last_selection);
        QCOMPARE(preparation.last_selection->generation, 7U);
        QCOMPARE(preparation.last_selection->candidate_id, "socketcan:can0");
    }

    void refreshReplacesCandidatesAndInvalidatesOldIds()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);
        require_selection(controller, preparation, 1, "old");
        preparation.next_refresh = snapshot(2, {"new"});

        controller.refreshAdapters();
        controller.connectWithAdapter("old");

        QCOMPARE(controller.discoveryGeneration(), 2ULL);
        QCOMPARE(controller.candidates()
                     ->data(controller.candidates()->index(0, 0), AdapterCandidateModel::CandidateIdRole)
                     .toString(),
                 "new");
        QCOMPARE(preparation.prepare_calls, 1);
    }

    void refreshFailureTransitionsToFailed()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);
        controller.setDocument(usable_document());
        preparation.next_refresh = std::unexpected(Error{ErrorKind::Internal, "provider crashed"});

        controller.refreshAdapters();

        QCOMPARE(controller.state(), ConnectionState::Failed);
        QCOMPARE(controller.statusText(), "Unable to refresh adapters");
        QCOMPARE(controller.technicalDetail(), "provider crashed");
    }

    void preparedRunWaitsForRunningStatus()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);

        connect_prepared(controller, preparation);
        QCOMPARE(engine.start_calls, 1);
        QCOMPARE(controller.selectedAdapterLabel(), "Linux CAN (can0)");

        engine.publishStatus(desktop::logging::LoggingStatus::Running);

        QCOMPARE(controller.state(), ConnectionState::Running);
    }

    void startFailureDestroysPreparedRun()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);
        const auto destroyed = std::make_shared<int>(0);
        preparation.next_preparation = prepared_connection(destroyed);
        engine.start_result = std::unexpected(Error{ErrorKind::Internal, "worker unavailable"});
        controller.setDocument(usable_document());

        controller.connectDashboard();

        QCOMPARE(controller.state(), ConnectionState::Failed);
        QCOMPARE(controller.technicalDetail(), "worker unavailable");
        QCOMPARE(*destroyed, 1);
    }

    void carNotRespondingRecoversWhenEngineRunsAgain()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);
        connect_prepared(controller, preparation);

        engine.publishStatus(desktop::logging::LoggingStatus::CarNotResponding);
        QCOMPARE(controller.state(), ConnectionState::CarNotResponding);
        engine.publishStatus(desktop::logging::LoggingStatus::Running);
        QCOMPARE(controller.state(), ConnectionState::Running);
    }

    void completionReasonsHaveStableSummaries_data()
    {
        QTest::addColumn<desktop::logging::SessionEndReason>("reason");
        QTest::addColumn<QString>("summary");
        QTest::newRow("stopped") << desktop::logging::SessionEndReason::StoppedByUser << QStringLiteral("Disconnected");
        QTest::newRow("handshake") << desktop::logging::SessionEndReason::HandshakeFailed
                                   << QStringLiteral("Unable to start CDBG logging");
        QTest::newRow("adapter") << desktop::logging::SessionEndReason::AdapterDisconnected
                                 << QStringLiteral("Adapter disconnected");
        QTest::newRow("runtime") << desktop::logging::SessionEndReason::RuntimeFailed
                                 << QStringLiteral("Logging stopped unexpectedly");
    }

    void completionReasonsHaveStableSummaries()
    {
        QFETCH(desktop::logging::SessionEndReason, reason);
        QFETCH(QString, summary);
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);
        connect_prepared(controller, preparation);

        engine.publishEnded(reason, "engine detail");

        QCOMPARE(controller.statusText(), summary);
        QCOMPARE(controller.technicalDetail(), "engine detail");
        QCOMPARE(controller.state(), reason == desktop::logging::SessionEndReason::StoppedByUser
                                         ? ConnectionState::Disconnected
                                         : ConnectionState::Failed);
    }

    void disconnectStopsOnceAndWaitsForJoinedCompletion()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);
        connect_prepared(controller, preparation);
        engine.publishStatus(desktop::logging::LoggingStatus::Running);

        controller.disconnectDashboard();
        controller.disconnectDashboard();

        QCOMPARE(controller.state(), ConnectionState::Disconnecting);
        QCOMPARE(engine.stop_calls, 1);
        engine.publishEnded(desktop::logging::SessionEndReason::StoppedByUser);
        QCOMPARE(controller.state(), ConnectionState::Disconnected);
    }

    void destructionStopsAnActiveEngineWithoutLateControllerChanges()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        int emitted_states = 0;
        {
            auto controller = std::make_unique<DashboardConnectionController>(preparation, engine);
            connect(controller.get(), &DashboardConnectionController::stateChanged, [&] { ++emitted_states; });
            connect_prepared(*controller, preparation);
            engine.publishStatus(desktop::logging::LoggingStatus::Running);
        }
        const int states_at_destruction = emitted_states;
        engine.publishEnded(desktop::logging::SessionEndReason::RuntimeFailed, "late");

        QCOMPARE(engine.stop_calls, 1);
        QCOMPARE(emitted_states, states_at_destruction);
    }

    void disconnectDuringConnectingInvalidatesThePendingPreparation()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        DashboardConnectionController controller(preparation, engine);
        controller.setDocument(usable_document());
        preparation.next_preparation = prepared_connection();
        connect(&controller, &DashboardConnectionController::stateChanged,
                [&]
                {
                    if (controller.state() == ConnectionState::Connecting)
                    {
                        controller.disconnectDashboard();
                    }
                });

        controller.connectDashboard();

        QCOMPARE(controller.state(), ConnectionState::Disconnecting);
        QCOMPARE(preparation.prepare_calls, 0);
        QCOMPARE(engine.start_calls, 0);
    }

    void reconnectFromStopCompletionPreservesTheNewRunForDestruction()
    {
        FakePreparationService preparation;
        FakeLoggingEngine engine;
        bool reconnected = false;
        {
            DashboardConnectionController controller(preparation, engine);
            connect_prepared(controller, preparation);
            engine.publishStatus(desktop::logging::LoggingStatus::Running);
            engine.publish_stop_completion = true;
            connect(&controller, &DashboardConnectionController::stateChanged,
                    [&]
                    {
                        if (controller.state() == ConnectionState::Disconnected && !reconnected)
                        {
                            reconnected = true;
                            preparation.next_preparation = prepared_connection();
                            controller.connectDashboard();
                        }
                    });

            controller.disconnectDashboard();

            QVERIFY(reconnected);
            QCOMPARE(controller.state(), ConnectionState::Connecting);
            QCOMPARE(engine.start_calls, 2);
            QCOMPARE(engine.stop_calls, 1);
        }

        QCOMPARE(engine.stop_calls, 2);
    }
};

} // namespace
} // namespace fastecu::desktop_quick

QTEST_MAIN(fastecu::desktop_quick::DashboardConnectionControllerTest)
#include "dashboard_connection_controller_test.moc"
