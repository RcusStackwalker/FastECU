#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QString>
#include <QtTest>

#include <memory>
#include <optional>
#include <utility>

#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"

namespace fastecu::desktop_quick
{
namespace
{

dashboard::DashboardDocument usable_document()
{
    return {.cards = {dashboard::DashboardCard{}}};
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

    int start_calls = 0;
    int stop_calls = 0;
};

class LoadedApplication
{
  public:
    LoadedApplication() : controller(preparation, logging)
    {
        loaded = load_root(engine, controller);
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
    QQmlApplicationEngine engine;
    bool loaded = false;
};

bool click(QObject *button)
{
    return button != nullptr && QMetaObject::invokeMethod(button, "click");
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
        QCOMPARE(connect_button->property("enabled").toBool(), false);
        QCOMPARE(connect_button->property("text").toString(), QStringLiteral("Connect"));
        QCOMPARE(application.find("connectionStatus")->property("text").toString(), QStringLiteral("Disconnected"));
        QVERIFY(application.find("selectedAdapterLabel") != nullptr);
        QCOMPARE(application.find("adapterPicker")->property("visible").toBool(), false);
        QVERIFY(application.find("refreshAdaptersButton") != nullptr);
        QVERIFY(application.find("connectionErrorDetail") != nullptr);
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
        QCOMPARE(application.find("connectButton")->property("enabled").toBool(), false);
        QCOMPARE(application.find("refreshAdaptersButton")->property("enabled").toBool(), false);
        application.logging.publishRunning();

        QCOMPARE(application.controller.state(), ConnectionState::Running);
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
