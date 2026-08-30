#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QAccessible>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QString>
#include <QtTest>

#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_controller.h"
#include "src/backend/ports/testing/fake_clock.h"

namespace fastecu::desktop_quick
{
namespace
{

dashboard::DashboardDocument usable_document()
{
    return {.cards = {dashboard::DashboardCard{}}};
}

dashboard::DashboardDocument two_card_document()
{
    return {
        .metadata = {.format_version = 1, .name = "Engine dashboard"},
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
                {.id = "rpm",
                 .channel_id = "CDBG_ENGINE_RPM",
                 .conversion_id = "rpm",
                 .title = "Tachometer",
                 .order = 0},
                {.id = "coolant", .channel_id = "CDBG_COOLANT_TEMP", .conversion_id = "temperature", .order = 1},
            },
    };
}

logging::LogSample sample(const char *channel_id, double numeric_value)
{
    return {.channel_id = channel_id, .numeric_value = numeric_value};
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

class NoOpProtocol final : public logging::LoggingProtocol
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
    return {.run = desktop::logging::LoggingRun{std::move(*session), std::make_unique<NoOpProtocol>()},
            .selected = {.candidate_id = "socketcan:can0",
                         .kind = dashboard::AdapterKind::SocketCan,
                         .vendor = "Linux",
                         .display_name = "can0",
                         .label = "Linux CAN (can0)"}};
}

class FakePreparationService final : public IConnectionPreparationService
{
  public:
    connection::ConnectionPreparationOutcome prepare_run(const dashboard::DashboardDocument&,
                                                         std::optional<connection::AdapterSelection> selection) override
    {
        ++prepare_calls;
        last_selection = std::move(selection);
        if (!next_preparation)
        {
            return Error{ErrorKind::Internal, "unexpected prepare"};
        }
        auto result = std::move(*next_preparation);
        next_preparation.reset();
        return result;
    }

    Result<connection::AdapterDiscoverySnapshot> refresh() override
    {
        ++refresh_calls;
        if (!next_refresh)
        {
            return snapshot(1, {});
        }
        auto result = std::move(*next_refresh);
        next_refresh.reset();
        return result;
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
        return {};
    }

    void stop() override
    {
        ++stop_calls;
    }

    bool isRunning() const override
    {
        return false;
    }

    void publishRunning()
    {
        emit statusChanged(desktop::logging::LoggingStatus::Running);
    }

    void publishSamples(QVector<logging::LogSample> samples)
    {
        emit valuesUpdated(std::move(samples));
    }

    int start_calls = 0;
    int stop_calls = 0;
};

class LoadedApplication
{
  public:
    LoadedApplication()
        : controller(preparation, logging), document(two_card_document()),
          presentation(document, logging, controller, clock)
    {
        controller.setDocument(document);
        loaded = load_root(engine, controller, presentation);
    }

    QObject *root() const
    {
        return engine.rootObjects().isEmpty() ? nullptr : engine.rootObjects().front();
    }

    QObject *find(const char *name) const
    {
        QObject *application_root = root();
        return application_root == nullptr ? nullptr : application_root->findChild<QObject *>(QString::fromUtf8(name));
    }

    FakePreparationService preparation;
    FakeLoggingEngine logging;
    DashboardConnectionController controller;
    FakeClock clock;
    dashboard::DashboardDocument document;
    DashboardController presentation;
    QQmlApplicationEngine engine;
    bool loaded = false;
};

class LoadErrorApplication
{
  public:
    LoadErrorApplication() : controller(preparation, logging)
    {
        presentation =
            DashboardController::fromLoadError(QStringLiteral("resource is malformed"), logging, controller, clock);
        loaded = load_root(engine, controller, *presentation);
    }

    QObject *root() const
    {
        return engine.rootObjects().isEmpty() ? nullptr : engine.rootObjects().front();
    }

    QObject *find(const char *name) const
    {
        QObject *application_root = root();
        return application_root == nullptr ? nullptr : application_root->findChild<QObject *>(QString::fromUtf8(name));
    }

    FakePreparationService preparation;
    FakeLoggingEngine logging;
    DashboardConnectionController controller;
    FakeClock clock;
    std::unique_ptr<DashboardController> presentation;
    QQmlApplicationEngine engine;
    bool loaded = false;
};

bool click(QObject *button)
{
    return button != nullptr && QMetaObject::invokeMethod(button, "click");
}

QList<QObject *> visual_children_named(QQuickItem *item, const QString& object_name)
{
    QList<QObject *> result;
    for (QQuickItem *child : item->childItems())
    {
        if (child->objectName() == object_name)
        {
            result.append(child);
        }
        result.append(visual_children_named(child, object_name));
    }
    return result;
}

class DesktopQuickApplicationTest : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    void disconnectedStateLoadsTheConnectionSurface()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        QCOMPARE(
            application.engine.rootContext()->contextProperty(QStringLiteral("dashboardConnection")).value<QObject *>(),
            &application.controller);
        QCOMPARE(application.engine.rootContext()
                     ->contextProperty(QStringLiteral("dashboardPresentation"))
                     .value<QObject *>(),
                 &application.presentation);
        QCOMPARE(application.controller.canConnect(), true);
        QCOMPARE(application.presentation.cards()->rowCount(), 2);
        QObject *root = application.root();
        QVERIFY(root != nullptr);
        QCOMPARE(root->objectName(), QStringLiteral("desktopQuickRoot"));
        QCOMPARE(root->property("title").toString(), QStringLiteral("OmniHaste"));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("navigationRail")) != nullptr);
        QVERIFY(root->findChild<QObject *>(QStringLiteral("dashboardNavigation")) != nullptr);
        QVERIFY(root->findChild<QObject *>(QStringLiteral("workspace")) != nullptr);
        QVERIFY(application.find("connectionPanel") != nullptr);
        QObject *connect_button = application.find("connectButton");
        QVERIFY(connect_button != nullptr);
        QCOMPARE(connect_button->property("enabled").toBool(), true);
        QCOMPARE(connect_button->property("text").toString(), QStringLiteral("Connect"));
        QCOMPARE(application.find("connectionStatus")->property("text").toString(), QStringLiteral("Disconnected"));
        QVERIFY(application.find("selectedAdapterLabel") != nullptr);
        QCOMPARE(application.find("adapterPicker")->property("visible").toBool(), false);
        QVERIFY(application.find("refreshAdaptersButton") != nullptr);
        QVERIFY(application.find("connectionErrorDetail") != nullptr);
    }

    void dashboardLoadFailureKeepsShellVisibleWithoutCardsOrConnection()
    {
        LoadErrorApplication application;
        QVERIFY(application.loaded);
        QObject *root = application.root();
        QVERIFY(root != nullptr);
        QCOMPARE(root->objectName(), QStringLiteral("desktopQuickRoot"));

        QObject *dashboard_view = application.find("dashboardView");
        QVERIFY(dashboard_view != nullptr);
        auto *dashboard_item = qobject_cast<QQuickItem *>(dashboard_view);
        QVERIFY(dashboard_item != nullptr);
        QCOMPARE(visual_children_named(dashboard_item, QStringLiteral("numericCard")).size(), 0);

        QObject *dashboard_load_error = application.find("loadErrorText");
        QVERIFY(dashboard_load_error != nullptr);
        QCOMPARE(visual_children_named(dashboard_item, QStringLiteral("loadErrorText")).size(), 1);
        QCOMPARE(dashboard_load_error->property("visible").toBool(), true);
        QCOMPARE(dashboard_load_error->property("text").toString(), QStringLiteral("resource is malformed"));

        QObject *connect_button = application.find("connectButton");
        QVERIFY(connect_button != nullptr);
        QCOMPARE(connect_button->property("enabled").toBool(), false);
    }

    void rejectedSamplesReachApplicationDiagnosticLog()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        QTest::ignoreMessage(QtWarningMsg, "Ignored sample for unknown dashboard channel 'unknown'");
        QTest::ignoreMessage(QtWarningMsg, "Ignored non-finite sample for dashboard channel 'CDBG_ENGINE_RPM'");

        application.logging.publishSamples(
            {sample("unknown", 42.0), sample("CDBG_ENGINE_RPM", std::numeric_limits<double>::infinity())});
    }

    void dashboardRendersCardsInModelOrderAndRetainsReadingsAcrossStates()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        QObject *dashboard_header = application.find("dashboardHeader");
        QVERIFY(dashboard_header != nullptr);
        auto *dashboard_header_item = qobject_cast<QQuickItem *>(dashboard_header);
        QVERIFY(dashboard_header_item != nullptr);
        QObject *dashboard_title = application.find("dashboardTitle");
        QVERIFY(dashboard_title != nullptr);
        QCOMPARE(dashboard_title->property("text").toString(), QStringLiteral("Engine dashboard"));
        QCOMPARE(visual_children_named(dashboard_header_item, QStringLiteral("dashboardTitle")).size(), 1);
        QObject *dashboard_status = application.find("dashboardHeaderStatus");
        QVERIFY(dashboard_status != nullptr);
        QCOMPARE(dashboard_status->property("text").toString(), QStringLiteral("Disconnected"));
        QAccessibleInterface *status_accessible = QAccessible::queryAccessibleInterface(dashboard_status);
        QVERIFY(status_accessible != nullptr);
        QCOMPARE(status_accessible->text(QAccessible::Name), QStringLiteral("Connection status: Disconnected"));

        QObject *dashboard_view = application.find("dashboardView");
        QVERIFY(dashboard_view != nullptr);
        auto *dashboard_item = qobject_cast<QQuickItem *>(dashboard_view);
        QVERIFY(dashboard_item != nullptr);
        QCOMPARE(visual_children_named(dashboard_item, QStringLiteral("dashboardTitle")).size(), 0);
        const QList<QObject *> cards = visual_children_named(dashboard_item, QStringLiteral("numericCard"));
        QCOMPARE(cards.size(), 2);
        for (QObject *card : cards)
        {
            QVERIFY(card->property("usesDashboardCardFrame").isValid());
            QCOMPARE(card->property("usesDashboardCardFrame").toBool(), true);
        }
        QVERIFY(cards.at(0)->property("waitingReadingState").isValid());
        QVERIFY(cards.at(0)->property("liveReadingState").isValid());
        QVERIFY(cards.at(0)->property("staleReadingState").isValid());
        QCOMPARE(cards.at(0)->property("waitingReadingState").toInt(), static_cast<int>(ReadingState::Waiting));
        QCOMPARE(cards.at(0)->property("liveReadingState").toInt(), static_cast<int>(ReadingState::Live));
        QCOMPARE(cards.at(0)->property("staleReadingState").toInt(), static_cast<int>(ReadingState::Stale));
        const QList<QObject *> titles = visual_children_named(dashboard_item, QStringLiteral("cardTitle"));
        QCOMPARE(titles.size(), 2);
        QCOMPARE(titles.at(0)->property("text").toString(), QStringLiteral("Tachometer"));
        QCOMPARE(titles.at(1)->property("text").toString(), QStringLiteral("Coolant Temperature"));

        const QList<QObject *> values = visual_children_named(dashboard_item, QStringLiteral("cardValue"));
        const QList<QObject *> units = visual_children_named(dashboard_item, QStringLiteral("cardUnit"));
        const QList<QObject *> states = visual_children_named(dashboard_item, QStringLiteral("cardState"));
        const QList<QObject *> ages = visual_children_named(dashboard_item, QStringLiteral("cardAge"));
        QCOMPARE(values.size(), 2);
        QCOMPARE(units.size(), 2);
        QCOMPARE(states.size(), 2);
        QCOMPARE(ages.size(), 2);
        QCOMPARE(values.at(0)->property("text").toString(), QString::fromUtf8("—"));
        QCOMPARE(values.at(1)->property("text").toString(), QString::fromUtf8("—"));
        QCOMPARE(units.at(0)->property("text").toString(), QStringLiteral("rpm"));
        QCOMPARE(states.at(0)->property("text").toString(), QStringLiteral("Waiting"));
        QCOMPARE(ages.at(0)->property("visible").toBool(), false);
        QAccessibleInterface *card_accessible = QAccessible::queryAccessibleInterface(cards.at(0));
        QVERIFY(card_accessible != nullptr);
        QCOMPARE(card_accessible->text(QAccessible::Name), QString::fromUtf8("Tachometer — rpm Waiting"));

        auto *cards_model = static_cast<DashboardCardModel *>(application.presentation.cards());
        cards_model->applySamples({sample("CDBG_ENGINE_RPM", 3125.4)}, 1000, true);
        QTRY_COMPARE(values.at(0)->property("text").toString(), QStringLiteral("3125"));
        QTRY_COMPARE(states.at(0)->property("text").toString(), QStringLiteral("Live"));
        QCOMPARE(card_accessible->text(QAccessible::Name), QStringLiteral("Tachometer 3125 rpm Live"));

        cards_model->markReceivedRowsStale();
        cards_model->updateAges(13000);
        QTRY_COMPARE(states.at(0)->property("text").toString(), QStringLiteral("Stale"));
        QCOMPARE(values.at(0)->property("text").toString(), QStringLiteral("3125"));
        QCOMPARE(ages.at(0)->property("visible").toBool(), true);
        QCOMPARE(ages.at(0)->property("text").toString(), QStringLiteral("Last update 12s ago"));
        QCOMPARE(card_accessible->text(QAccessible::Name), QStringLiteral("Tachometer 3125 rpm Stale"));
    }

    void dashboardGridUsesResponsiveColumnsWithoutReorderingCards()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        QObject *root = application.root();
        QObject *grid = application.find("dashboardGrid");
        QVERIFY(grid != nullptr);
        root->setProperty("minimumWidth", 0);
        root->setProperty("width", 800);
        QTRY_VERIFY(grid->property("columns").toInt() >= 2);
        root->setProperty("width", 480);
        QTRY_COMPARE(grid->property("columns").toInt(), 1);

        QObject *dashboard_view = application.find("dashboardView");
        QVERIFY(dashboard_view != nullptr);
        auto *dashboard_item = qobject_cast<QQuickItem *>(dashboard_view);
        QVERIFY(dashboard_item != nullptr);
        const QList<QObject *> titles = visual_children_named(dashboard_item, QStringLiteral("cardTitle"));
        QCOMPARE(titles.size(), 2);
        QCOMPARE(titles.at(0)->property("text").toString(), QStringLiteral("Tachometer"));
        QCOMPARE(titles.at(1)->property("text").toString(), QStringLiteral("Coolant Temperature"));
    }

    void connectButtonRequestsAdapterSelection()
    {
        LoadedApplication application;
        application.controller.setDocument(usable_document());
        application.preparation.next_preparation = connection::AdapterSelectionRequired{
            .snapshot = snapshot(5, {"socketcan:can0"}),
            .reason = connection::AdapterSelectionRequired::Reason::NoPreference,
        };

        QVERIFY(click(application.find("connectButton")));

        QCOMPARE(application.preparation.prepare_calls, 1);
        QCOMPARE(application.controller.state(), ConnectionState::AdapterSelectionRequired);
        QCOMPARE(application.find("connectionPanel")->property("transitioning").toBool(), false);
        QCOMPARE(application.find("connectionStatus")->property("text").toString(),
                 QStringLiteral("Select an adapter"));
        QCOMPARE(application.find("adapterPicker")->property("visible").toBool(), true);
    }

    void pickerRefreshAndConfirmationCallControllerActions()
    {
        LoadedApplication application;
        application.controller.setDocument(usable_document());
        application.preparation.next_preparation = connection::AdapterSelectionRequired{
            .snapshot = snapshot(5, {"socketcan:old"}),
            .reason = connection::AdapterSelectionRequired::Reason::NoPreference,
        };
        QVERIFY(click(application.find("connectButton")));
        application.preparation.next_refresh = snapshot(6, {"socketcan:can0"});

        QVERIFY(click(application.find("refreshAdaptersButton")));

        QCOMPARE(application.preparation.refresh_calls, 1);
        QObject *adapter_picker = application.find("adapterPicker");
        QObject *confirm_button = application.find("confirmAdapterButton");
        QCOMPARE(adapter_picker->property("count").toInt(), 1);
        QCOMPARE(adapter_picker->property("currentIndex").toInt(), 0);
        QCOMPARE(adapter_picker->property("currentValue").toString(), QStringLiteral("socketcan:can0"));
        QCOMPARE(confirm_button->property("enabled").toBool(), true);
        application.preparation.next_preparation = Error{ErrorKind::Internal, "open failed"};
        QVERIFY(click(confirm_button));
        QCOMPARE(application.preparation.prepare_calls, 2);
        QVERIFY(application.preparation.last_selection.has_value());
        QCOMPARE(application.preparation.last_selection->generation, 6U);
        QCOMPARE(application.preparation.last_selection->candidate_id, "socketcan:can0");
    }

    void runningStateOffersDisconnectAndShowsSelectedAdapter()
    {
        LoadedApplication application;
        application.controller.setDocument(usable_document());
        application.preparation.next_preparation = prepared_connection();
        QVERIFY(click(application.find("connectButton")));
        QCOMPARE(application.logging.start_calls, 1);
        QCOMPARE(application.find("connectionPanel")->property("transitioning").toBool(), true);
        QCOMPARE(application.find("connectButton")->property("enabled").toBool(), false);
        QCOMPARE(application.find("refreshAdaptersButton")->property("enabled").toBool(), false);
        application.logging.publishRunning();

        QCOMPARE(application.controller.state(), ConnectionState::Running);
        QObject *dashboard_status = application.find("dashboardHeaderStatus");
        QVERIFY(dashboard_status != nullptr);
        QCOMPARE(dashboard_status->property("text").toString(), QStringLiteral("Connected"));
        QCOMPARE(application.find("connectButton")->property("text").toString(), QStringLiteral("Disconnect"));
        QCOMPARE(application.find("selectedAdapterLabel")->property("text").toString(),
                 QStringLiteral("Adapter: Linux CAN (can0)"));

        QVERIFY(click(application.find("connectButton")));
        QCOMPARE(application.logging.stop_calls, 1);
        QCOMPARE(application.controller.state(), ConnectionState::Disconnecting);
        QCOMPARE(application.find("connectButton")->property("enabled").toBool(), false);
    }

    void failedStateKeepsDetailCollapsedUntilExpanded()
    {
        LoadedApplication application;
        application.controller.setDocument(usable_document());
        application.preparation.next_preparation = Error{ErrorKind::Disconnected, "driver returned code 7"};
        QVERIFY(click(application.find("connectButton")));

        QCOMPARE(application.controller.state(), ConnectionState::Failed);
        QCOMPARE(application.find("connectionStatus")->property("text").toString(),
                 QStringLiteral("Unable to prepare logging"));
        QObject *detail = application.find("connectionErrorDetail");
        QVERIFY(detail != nullptr);
        QCOMPARE(detail->property("visible").toBool(), false);
        QCOMPARE(detail->property("text").toString(), QStringLiteral("driver returned code 7"));

        QVERIFY(click(application.find("connectionErrorToggle")));
        QCOMPARE(detail->property("visible").toBool(), true);
    }
};
} // namespace
} // namespace fastecu::desktop_quick

QTEST_MAIN(fastecu::desktop_quick::DesktopQuickApplicationTest)
#include "desktop_quick_application_test.moc"
