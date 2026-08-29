#include <QPointer>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>

#include <limits>
#include <memory>
#include <utility>

#include "src/backend/ports/testing/fake_clock.h"
#include "src/ui/desktop-quick/dashboard/dashboard_controller.h"

namespace fastecu::desktop_quick
{
namespace
{

logging::LogSample sample(const char *channel_id, double numeric_value)
{
    return {.channel_id = channel_id, .numeric_value = numeric_value};
}

dashboard::DashboardDocument dashboard_document()
{
    return {
        .metadata = {.format_version = 1, .name = "Colt CDBG Dashboard"},
        .channels =
            {
                {.id = "CDBG_ENGINE_RPM",
                 .name = "Engine RPM",
                 .conversions = {{.id = "rpm", .unit = "rpm", .precision = 0}}},
                {.id = "CDBG_COOLANT_TEMP",
                 .name = "Coolant Temperature",
                 .conversions = {{.id = "temperature", .unit = "°C", .precision = 1}}},
            },
        .cards =
            {
                {.id = "rpm", .channel_id = "CDBG_ENGINE_RPM", .conversion_id = "rpm", .order = 0},
                {.id = "coolant", .channel_id = "CDBG_COOLANT_TEMP", .conversion_id = "temperature", .order = 1},
            },
    };
}

class NoopProtocol final : public logging::LoggingProtocol
{
  public:
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
};

connection::PreparedConnection prepared_connection()
{
    auto session = logging::make_logging_session(
        logging::LoggingProtocolId::Cdbg,
        {logging::LoggingChannel{.id = "CDBG_ENGINE_RPM",
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
    return {.run = desktop::logging::LoggingRun{std::move(*session), std::make_unique<NoopProtocol>()},
            .selected = {.candidate_id = "socketcan:can0",
                         .kind = dashboard::AdapterKind::SocketCan,
                         .vendor = "Linux",
                         .display_name = "can0",
                         .label = "Linux CAN (can0)"}};
}

connection::AdapterDiscoverySnapshot selection_snapshot()
{
    return {.generation = 1,
            .candidates = {{.candidate_id = "socketcan:can0",
                            .kind = dashboard::AdapterKind::SocketCan,
                            .vendor = "Linux",
                            .display_name = "can0",
                            .label = "Linux CAN (can0)"}},
            .diagnostics = {}};
}

class FakePreparationService final : public IConnectionPreparationService
{
  public:
    connection::ConnectionPreparationOutcome prepare_run(const dashboard::DashboardDocument&,
                                                         std::optional<connection::AdapterSelection>) override
    {
        if (require_selection)
        {
            return connection::AdapterSelectionRequired{
                .snapshot = selection_snapshot(),
                .reason = connection::AdapterSelectionRequired::Reason::NoPreference,
            };
        }
        return prepared_connection();
    }

    Result<connection::AdapterDiscoverySnapshot> refresh() override
    {
        return selection_snapshot();
    }

    bool require_selection = false;
};

class FakeLoggingEngine final : public ILoggingEngine
{
  public:
    Status start(desktop::logging::LoggingRun) override
    {
        running = true;
        return {};
    }

    void stop() override
    {
        running = false;
    }

    bool isRunning() const override
    {
        return running;
    }

    void publishValues(QVector<logging::LogSample> samples)
    {
        emit valuesUpdated(std::move(samples));
    }

    void publishStatus(desktop::logging::LoggingStatus status)
    {
        emit statusChanged(status);
    }

    void publishEnded(desktop::logging::SessionEndReason reason, QString detail = {})
    {
        running = false;
        emit sessionEnded(reason, std::move(detail));
    }

    bool running = false;
};

struct Harness
{
    Harness() : connection(preparation, engine)
    {
        connection.setDocument(dashboard_document());
    }

    void connectRunning()
    {
        connection.connectDashboard();
        QCOMPARE(connection.state(), ConnectionState::Connecting);
        engine.publishStatus(desktop::logging::LoggingStatus::Running);
        QCOMPARE(connection.state(), ConnectionState::Running);
    }

    FakePreparationService preparation;
    FakeLoggingEngine engine;
    DashboardConnectionController connection;
    FakeClock clock;
};

QVariant role(const DashboardController& dashboard, int row, DashboardCardModel::Role role)
{
    QAbstractItemModel *cards = dashboard.cards();
    return cards->data(cards->index(row, 0), role);
}

ReadingState state(const DashboardController& dashboard, int row)
{
    return role(dashboard, row, DashboardCardModel::ReadingStateRole).value<ReadingState>();
}

QString card_value(const DashboardController& dashboard, int row)
{
    return role(dashboard, row, DashboardCardModel::FormattedValueRole).toString();
}

int notification_count(const QSignalSpy& changed, DashboardCardModel::Role role)
{
    int count = 0;
    for (const QList<QVariant>& notification : changed)
    {
        if (notification.at(2).value<QList<int>>().contains(role))
        {
            ++count;
        }
    }
    return count;
}

void flush_pending(DashboardController& dashboard)
{
    QVERIFY(QMetaObject::invokeMethod(&dashboard, "flushPendingSamples", Qt::DirectConnection));
}

void update_ages(DashboardController& dashboard)
{
    QVERIFY(QMetaObject::invokeMethod(&dashboard, "updateAges", Qt::DirectConnection));
}

QTimer *timer_with_interval(DashboardController& dashboard, int interval_ms)
{
    const auto timers = dashboard.findChildren<QTimer *>();
    for (QTimer *timer : timers)
    {
        if (timer->interval() == interval_ms)
        {
            return timer;
        }
    }
    return nullptr;
}

class DashboardControllerTest final : public QObject
{
    Q_OBJECT

  private slots:
    void exposesDocumentPresentationInWaitingStateWithoutArmedTimers()
    {
        Harness harness;
        DashboardController dashboard(dashboard_document(), harness.engine, harness.connection, harness.clock);

        QCOMPARE(dashboard.dashboardTitle(), QStringLiteral("Colt CDBG Dashboard"));
        QCOMPARE(dashboard.hasLoadError(), false);
        QCOMPARE(dashboard.loadErrorText(), QString{});
        QCOMPARE(dashboard.cards()->rowCount(), 2);
        QCOMPARE(card_value(dashboard, 0), QString::fromUtf8("—"));
        QCOMPARE(state(dashboard, 0), ReadingState::Waiting);
        QVERIFY(timer_with_interval(dashboard, 33));
        QVERIFY(!timer_with_interval(dashboard, 33)->isActive());
        QVERIFY(timer_with_interval(dashboard, 1000));
        QVERIFY(!timer_with_interval(dashboard, 1000)->isActive());
    }

    void loadErrorExposesNoPartialCards()
    {
        Harness harness;

        auto dashboard = DashboardController::fromLoadError(QStringLiteral("resource is malformed"), harness.engine,
                                                            harness.connection, harness.clock);

        QVERIFY(dashboard);
        QCOMPARE(dashboard->dashboardTitle(), QString{});
        QCOMPARE(dashboard->hasLoadError(), true);
        QCOMPARE(dashboard->loadErrorText(), QStringLiteral("resource is malformed"));
        QCOMPARE(dashboard->cards()->rowCount(), 0);
        QVERIFY(!timer_with_interval(*dashboard, 1000)->isActive());
    }

    void coalescesToTheNewestChannelValueInOneTimerFlush()
    {
        Harness harness;
        DashboardController dashboard(dashboard_document(), harness.engine, harness.connection, harness.clock);
        harness.connectRunning();
        QSignalSpy changed(dashboard.cards(), &QAbstractItemModel::dataChanged);

        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 1000.0)});
        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 2000.0)});

        QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 100);
        QCOMPARE(card_value(dashboard, 0), QStringLiteral("2000"));
        QCOMPARE(notification_count(changed, DashboardCardModel::FormattedValueRole), 1);
        QCOMPARE(state(dashboard, 0), ReadingState::Live);
    }

    void silenceStalesReceivedRowsImmediatelyAndFreshSamplesResumeOnlyTheirRows()
    {
        Harness harness;
        DashboardController dashboard(dashboard_document(), harness.engine, harness.connection, harness.clock);
        harness.connectRunning();
        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 2100.0), sample("CDBG_COOLANT_TEMP", 87.5)});
        flush_pending(dashboard);

        harness.engine.publishStatus(desktop::logging::LoggingStatus::CarNotResponding);

        QCOMPARE(state(dashboard, 0), ReadingState::Stale);
        QCOMPARE(state(dashboard, 1), ReadingState::Stale);
        harness.engine.publishStatus(desktop::logging::LoggingStatus::Running);
        QCOMPARE(state(dashboard, 0), ReadingState::Stale);
        QCOMPARE(state(dashboard, 1), ReadingState::Stale);
        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 2200.0)});
        flush_pending(dashboard);
        QCOMPARE(state(dashboard, 0), ReadingState::Live);
        QCOMPARE(state(dashboard, 1), ReadingState::Stale);
    }

    void terminalStateFlushesNewestPendingValueBeforeRetainingItAsStale()
    {
        Harness harness;
        DashboardController dashboard(dashboard_document(), harness.engine, harness.connection, harness.clock);
        harness.connectRunning();
        QSignalSpy changed(dashboard.cards(), &QAbstractItemModel::dataChanged);
        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 3000.0)});
        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 3250.0)});

        harness.engine.publishEnded(desktop::logging::SessionEndReason::RuntimeFailed, QStringLiteral("lost"));

        QCOMPARE(harness.connection.state(), ConnectionState::Failed);
        QCOMPARE(card_value(dashboard, 0), QStringLiteral("3250"));
        QCOMPARE(state(dashboard, 0), ReadingState::Stale);
        QCOMPARE(notification_count(changed, DashboardCardModel::FormattedValueRole), 1);
        QVERIFY(!timer_with_interval(dashboard, 33)->isActive());
    }

    void reconnectKeepsOldValuesStaleUntilAValidSampleArrives()
    {
        Harness harness;
        DashboardController dashboard(dashboard_document(), harness.engine, harness.connection, harness.clock);
        harness.connectRunning();
        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 1800.0)});
        flush_pending(dashboard);
        harness.engine.publishEnded(desktop::logging::SessionEndReason::AdapterDisconnected);
        QCOMPARE(state(dashboard, 0), ReadingState::Stale);

        harness.connection.connectDashboard();
        QCOMPARE(harness.connection.state(), ConnectionState::Connecting);
        QCOMPARE(state(dashboard, 0), ReadingState::Stale);
        harness.engine.publishStatus(desktop::logging::LoggingStatus::Running);
        QCOMPARE(state(dashboard, 0), ReadingState::Stale);
        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 1900.0)});
        flush_pending(dashboard);

        QCOMPARE(card_value(dashboard, 0), QStringLiteral("1900"));
        QCOMPARE(state(dashboard, 0), ReadingState::Live);
    }

    void connectingAndAdapterSelectionDoNotEraseOrReviveAStaleValue()
    {
        Harness harness;
        DashboardController dashboard(dashboard_document(), harness.engine, harness.connection, harness.clock);
        harness.connectRunning();
        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 1750.0)});
        flush_pending(dashboard);
        harness.engine.publishEnded(desktop::logging::SessionEndReason::RuntimeFailed);
        harness.preparation.require_selection = true;

        harness.connection.connectDashboard();

        QCOMPARE(harness.connection.state(), ConnectionState::AdapterSelectionRequired);
        QCOMPARE(card_value(dashboard, 0), QStringLiteral("1750"));
        QCOMPARE(state(dashboard, 0), ReadingState::Stale);
    }

    void fakeClockDrivesAgeUpdatesOnlyAfterTheFirstReading()
    {
        Harness harness;
        harness.clock.now_ = 1000;
        DashboardController dashboard(dashboard_document(), harness.engine, harness.connection, harness.clock);
        QTimer *age_timer = timer_with_interval(dashboard, 1000);
        QVERIFY(age_timer);
        QVERIFY(!age_timer->isActive());
        harness.connectRunning();
        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 2000.0)});
        flush_pending(dashboard);
        QVERIFY(age_timer->isActive());
        QCOMPARE(role(dashboard, 0, DashboardCardModel::LastUpdateAgeTextRole), QStringLiteral("Last update 0s ago"));
        QSignalSpy changed(dashboard.cards(), &QAbstractItemModel::dataChanged);

        harness.clock.now_ = 3500;
        update_ages(dashboard);
        update_ages(dashboard);

        QCOMPARE(role(dashboard, 0, DashboardCardModel::LastUpdateAgeTextRole), QStringLiteral("Last update 2s ago"));
        QCOMPARE(notification_count(changed, DashboardCardModel::LastUpdateAgeTextRole), 1);
    }

    void unknownAndNonFiniteSamplesEmitDiagnosticsWithoutChangingCardsOrStartingTimers()
    {
        Harness harness;
        DashboardController dashboard(dashboard_document(), harness.engine, harness.connection, harness.clock);
        QSignalSpy diagnostics(&dashboard, &DashboardController::diagnostic);
        QSignalSpy changed(dashboard.cards(), &QAbstractItemModel::dataChanged);

        harness.engine.publishValues(
            {sample("unknown", 42.0), sample("CDBG_ENGINE_RPM", std::numeric_limits<double>::infinity())});

        QCOMPARE(diagnostics.count(), 2);
        QVERIFY(diagnostics.at(0).at(0).toString().contains(QStringLiteral("unknown")));
        QVERIFY(diagnostics.at(1).at(0).toString().contains(QStringLiteral("non-finite")));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(card_value(dashboard, 0), QString::fromUtf8("—"));
        QVERIFY(!timer_with_interval(dashboard, 33)->isActive());
        QVERIFY(!timer_with_interval(dashboard, 1000)->isActive());
    }

    void destructionIsSafeWithBothTimersArmedAndPendingSamples()
    {
        Harness harness;
        QPointer<DashboardController> guard;
        {
            auto dashboard = std::make_unique<DashboardController>(dashboard_document(), harness.engine,
                                                                   harness.connection, harness.clock);
            guard = dashboard.get();
            harness.connectRunning();
            harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 2000.0)});
            flush_pending(*dashboard);
            QVERIFY(timer_with_interval(*dashboard, 1000)->isActive());
            harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 2100.0)});
            QVERIFY(timer_with_interval(*dashboard, 33)->isActive());
        }

        QVERIFY(guard.isNull());
        harness.engine.publishValues({sample("CDBG_ENGINE_RPM", 2200.0)});
        QTest::qWait(40);
        QVERIFY(guard.isNull());
    }
};

} // namespace
} // namespace fastecu::desktop_quick

QTEST_GUILESS_MAIN(fastecu::desktop_quick::DashboardControllerTest)
#include "dashboard_controller_test.moc"
