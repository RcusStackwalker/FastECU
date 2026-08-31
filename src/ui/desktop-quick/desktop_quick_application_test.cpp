#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QAccessible>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QString>
#include <QWindow>
#include <QtTest>

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/dashboard/dashboard_codec.h"
#include "src/backend/dashboard/dashboard_document_service.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_settings.h"
#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_document_controller.h"
#include "src/ui/desktop-quick/dashboard/dashboard_editor_model.h"

namespace fastecu::desktop_quick
{
namespace
{

constexpr std::string_view kLifecycleCatalog = R"(<logger><protocols><protocol id="CDBG"><parameters>
  <parameter id="CDBG_ENGINE_RPM" name="Engine RPM" desc="Engine speed" length="2" enabled="1">
    <address>0x100</address><conversions>
      <conversion units="rpm" expr="x" format="0" gauge_min="0" gauge_max="8000" gauge_step="500"/>
    </conversions>
  </parameter>
  <parameter id="CDBG_COOLANT_TEMP" name="Coolant Temperature" desc="Coolant" length="2" enabled="1">
    <address>0x102</address><conversions>
      <conversion units="C" expr="x" format="0.0" gauge_min="-40" gauge_max="260" gauge_step="10"/>
    </conversions>
  </parameter>
  <parameter id="CDBG_MANIFOLD_PRESSURE" name="Manifold Pressure" desc="Pressure" length="2" enabled="1">
    <address>0x104</address><conversions>
      <conversion units="kPa" expr="x" format="0" gauge_min="0" gauge_max="250" gauge_step="5"/>
    </conversions>
  </parameter>
</parameters></protocol></protocols></logger>)";

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return {text.begin(), text.end()};
}

dashboard::LegacyCdbgImportDefaults lifecycle_import_defaults()
{
    return dashboard::LegacyCdbgImportDefaults{
        .document_name = "Imported lifecycle dashboard",
        .bitrate = 500000,
        .identifier_width = dashboard::CanIdentifierWidth::Standard,
        .stream_instance = 0,
        .sampling_interval_ms = 50,
        .retry = dashboard::RetryPolicy{100, 3, 3, 250},
    };
}

class InMemoryDashboardStore final : public IFileRepository, public IAtomicFileWriter
{
  public:
    Result<std::vector<std::uint8_t>> read(std::string_view handle) override
    {
        const auto found = files.find(std::string(handle));
        if (found == files.end())
        {
            return fail(ErrorKind::InvalidConfig, "no such handle");
        }
        return found->second;
    }

    Status write(std::string_view handle, std::span<const std::uint8_t> data) override
    {
        files[std::string(handle)] = {data.begin(), data.end()};
        return {};
    }

    Status replace(std::string_view handle, std::span<const std::uint8_t> data) override
    {
        return write(handle, data);
    }

    std::map<std::string, std::vector<std::uint8_t>> files;
};

dashboard::DashboardDocument usable_document()
{
    return {.cards = {dashboard::DashboardCard{}}};
}

dashboard::DashboardDocument mixed_card_document()
{
    return {
        .metadata = {.format_version = 1, .name = "Engine dashboard"},
        .connection =
            {
                .protocol = dashboard::DashboardProtocol::Cdbg,
                .transport = dashboard::DashboardTransport::RawCan,
                .bitrate = 500000,
                .identifier_width = dashboard::CanIdentifierWidth::Standard,
                .request_id = 0x630,
                .reply_id = 0x631,
                .stream_instance = 0,
                .sampling_interval_ms = 50,
                .retry = dashboard::RetryPolicy{100, 3, 3, 250},
            },
        .channels =
            {
                {.id = "CDBG_ENGINE_RPM",
                 .name = "Engine RPM",
                 .description = "Engine speed",
                 .address = 0x100,
                 .length = 2,
                 .raw_assembly = dashboard::RawAssembly::UnsignedIntegerDecimal,
                 .conversions = {{.id = "rpm",
                                  .expression = "x",
                                  .unit = "rpm",
                                  .precision = 0,
                                  .gauge_min = 0.0,
                                  .gauge_max = 8000.0,
                                  .gauge_step = 500.0}}},
                {.id = "CDBG_COOLANT_TEMP",
                 .name = "Coolant Temperature",
                 .description = "Engine coolant temperature",
                 .address = 0x102,
                 .length = 2,
                 .raw_assembly = dashboard::RawAssembly::UnsignedIntegerDecimal,
                 .conversions = {{.id = "temperature",
                                  .expression = "x",
                                  .unit = "°C",
                                  .precision = 1,
                                  .gauge_min = -40.0,
                                  .gauge_max = 260.0,
                                  .gauge_step = 10.0}}},
                {.id = "CDBG_MANIFOLD_PRESSURE",
                 .name = "Manifold Pressure",
                 .description = "Intake manifold pressure",
                 .address = 0x104,
                 .length = 2,
                 .raw_assembly = dashboard::RawAssembly::UnsignedIntegerDecimal,
                 .conversions = {{.id = "pressure",
                                  .expression = "x",
                                  .unit = "kPa",
                                  .precision = 0,
                                  .gauge_min = 0.0,
                                  .gauge_max = 100.0,
                                  .gauge_step = 5.0}}},
            },
        .cards =
            {
                {.id = "rpm",
                 .channel_id = "CDBG_ENGINE_RPM",
                 .conversion_id = "rpm",
                 .display_type = dashboard::CardDisplayType::Numeric,
                 .title = "Tachometer",
                 .order = 0},
                {.id = "coolant",
                 .channel_id = "CDBG_COOLANT_TEMP",
                 .conversion_id = "temperature",
                 .display_type = dashboard::CardDisplayType::Sparkline,
                 .order = 1,
                 .sparkline_history_seconds = 30},
                {.id = "manifold-pressure",
                 .channel_id = "CDBG_MANIFOLD_PRESSURE",
                 .conversion_id = "pressure",
                 .display_type = dashboard::CardDisplayType::HorizontalGauge,
                 .order = 2,
                 .gauge_bounds = dashboard::GaugeBoundsOverride{0.0, 100.0, 5.0}},
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
        : documents(repository, writer), document_controller(documents, settings), editor(document_controller),
          controller(preparation, logging), document(mixed_card_document()),
          presentation(std::nullopt, logging, controller, clock)
    {
        auto encoded = dashboard::encode_dashboard_document(document);
        if (!encoded.has_value())
        {
            qFatal("Failed to encode application fixture: %s", encoded.error().detail.c_str());
        }
        repository.files["editable.ohd"] = std::move(*encoded);
        const Status opened = document_controller.openDocument("editable.ohd");
        if (!opened.has_value())
        {
            qFatal("Failed to open application fixture: %s", opened.error().detail.c_str());
        }
        loaded = load_root(engine, controller, presentation, document_controller, editor);
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

    InMemoryFileRepository repository;
    InMemoryAtomicFileWriter writer;
    InMemorySettings settings;
    dashboard::DashboardDocumentService documents;
    DashboardDocumentController document_controller;
    DashboardEditorModel editor;
    FakePreparationService preparation;
    FakeLoggingEngine logging;
    DashboardConnectionController controller;
    FakeClock clock;
    dashboard::DashboardDocument document;
    DashboardController presentation;
    QQmlApplicationEngine engine;
    bool loaded = false;
};

class EmptyApplication
{
  public:
    EmptyApplication()
        : documents(repository, writer), document_controller(documents, settings), editor(document_controller),
          controller(preparation, logging), presentation(std::nullopt, logging, controller, clock)
    {
        loaded = load_root(engine, controller, presentation, document_controller, editor);
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

    InMemoryFileRepository repository;
    InMemoryAtomicFileWriter writer;
    InMemorySettings settings;
    dashboard::DashboardDocumentService documents;
    DashboardDocumentController document_controller;
    DashboardEditorModel editor;
    FakePreparationService preparation;
    FakeLoggingEngine logging;
    DashboardConnectionController controller;
    FakeClock clock;
    DashboardController presentation;
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

QList<QObject *> visual_dashboard_cards(QQuickItem *item)
{
    QList<QObject *> result;
    for (QQuickItem *child : item->childItems())
    {
        if (child->objectName() == QStringLiteral("numericCard") ||
            child->objectName() == QStringLiteral("sparklineCard") ||
            child->objectName() == QStringLiteral("horizontalGaugeCard"))
        {
            result.append(child);
        }
        result.append(visual_dashboard_cards(child));
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
        QGuiApplication::setQuitOnLastWindowClosed(false);
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
        QCOMPARE(
            application.engine.rootContext()->contextProperty(QStringLiteral("dashboardDocuments")).value<QObject *>(),
            &application.document_controller);
        QCOMPARE(
            application.engine.rootContext()->contextProperty(QStringLiteral("dashboardEditor")).value<QObject *>(),
            &application.editor);
        QCOMPARE(application.controller.canConnect(), true);
        QCOMPARE(application.presentation.cards()->rowCount(), 3);
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

    void committedDocumentFansOutExactlyOnceToEveryApplicationProjection()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        QSignalSpy presentation_reset(&application.presentation, &DashboardController::documentChanged);
        QSignalSpy connection_reset(&application.controller, &DashboardConnectionController::stateChanged);
        QSignalSpy editor_reset(&application.editor, &QAbstractItemModel::modelReset);
        dashboard::DashboardDocument candidate = *application.document_controller.document();
        candidate.cards.clear();

        const Status committed = application.document_controller.commitCandidate(std::move(candidate), {});

        QVERIFY2(committed.has_value(), committed.has_value() ? "" : committed.error().detail.c_str());
        QCOMPARE(presentation_reset.count(), 1);
        QCOMPARE(connection_reset.count(), 1);
        QCOMPARE(editor_reset.count(), 1);
        QCOMPARE(application.presentation.cards()->rowCount(), 0);
        QCOMPARE(application.controller.canConnect(), false);
        QCOMPARE(application.editor.rowCount(), 0);
    }

    void importEditSaveAndRestoreRoundTripsEveryCardType()
    {
        InMemoryDashboardStore store;
        store.files["catalog.xml"] = bytes_of(kLifecycleCatalog);
        InMemorySettings settings;
        dashboard::DashboardDocumentService documents(store, store);
        DashboardDocumentController document_controller(documents, settings);
        DashboardEditorModel editor(document_controller);
        FakePreparationService preparation;
        FakeLoggingEngine logging;
        DashboardConnectionController connection_controller(preparation, logging);
        FakeClock clock;
        DashboardController presentation(std::nullopt, logging, connection_controller, clock);
        QQmlApplicationEngine engine;
        QVERIFY(load_root(engine, connection_controller, presentation, document_controller, editor));

        const Status imported = document_controller.importDocument("catalog.xml", lifecycle_import_defaults());
        QVERIFY2(imported.has_value(), imported.has_value() ? "" : imported.error().detail.c_str());
        editor.addCard(QStringLiteral("CDBG_ENGINE_RPM"), QStringLiteral("conversion-1"));
        editor.addCard(QStringLiteral("CDBG_COOLANT_TEMP"), QStringLiteral("conversion-1"));
        editor.setSelectedDisplayType(CardDisplayType::HorizontalGauge);
        editor.setSelectedGaugeBounds(-20.0, 240.0, 5.0);
        editor.setSelectedDisplayType(CardDisplayType::Sparkline);
        editor.setSelectedSparklineHistorySeconds(45);
        editor.addCard(QStringLiteral("CDBG_MANIFOLD_PRESSURE"), QStringLiteral("conversion-1"));
        editor.setSelectedDisplayType(CardDisplayType::Sparkline);
        editor.setSelectedSparklineHistorySeconds(90);
        editor.setSelectedDisplayType(CardDisplayType::HorizontalGauge);
        editor.setSelectedGaugeBounds(10.0, 200.0, 5.0);
        editor.moveSelectedUp();
        QCOMPARE(presentation.cards()->rowCount(), 3);
        QCOMPARE(connection_controller.canConnect(), true);

        const Status saved = document_controller.saveAs("edited.ohd");
        QVERIFY2(saved.has_value(), saved.has_value() ? "" : saved.error().detail.c_str());
        QCOMPARE(settings.get(DashboardDocumentController::recentPathKey), std::optional<std::string>{"edited.ohd"});

        dashboard::DashboardDocumentService restored_documents(store, store);
        DashboardDocumentController restored_controller(restored_documents, settings);
        DashboardEditorModel restored_editor(restored_controller);
        FakePreparationService restored_preparation;
        FakeLoggingEngine restored_logging;
        DashboardConnectionController restored_connection(restored_preparation, restored_logging);
        FakeClock restored_clock;
        DashboardController restored_presentation(std::nullopt, restored_logging, restored_connection, restored_clock);
        const Status restored = restored_controller.restoreRecentDocument();
        QVERIFY2(restored.has_value(), restored.has_value() ? "" : restored.error().detail.c_str());
        QQmlApplicationEngine restored_engine;
        QVERIFY(load_root(restored_engine, restored_connection, restored_presentation, restored_controller,
                          restored_editor));

        QVERIFY(restored_controller.document().has_value());
        const dashboard::DashboardDocument& document = *restored_controller.document();
        QCOMPARE(document.metadata.name, std::string{"Imported lifecycle dashboard"});
        QCOMPARE(document.cards.size(), std::size_t{3});
        QCOMPARE(document.cards[0].id, std::string{"CDBG_ENGINE_RPM-1"});
        QCOMPARE(document.cards[0].order, std::uint32_t{0});
        QCOMPARE(document.cards[0].display_type, dashboard::CardDisplayType::Numeric);
        QVERIFY(!document.cards[0].gauge_bounds.has_value());
        QVERIFY(!document.cards[0].sparkline_history_seconds.has_value());
        QCOMPARE(document.cards[1].id, std::string{"CDBG_MANIFOLD_PRESSURE-1"});
        QCOMPARE(document.cards[1].order, std::uint32_t{1});
        QCOMPARE(document.cards[1].display_type, dashboard::CardDisplayType::HorizontalGauge);
        const std::optional<dashboard::GaugeBoundsOverride> expected_gauge{{10.0, 200.0, 5.0}};
        QCOMPARE(document.cards[1].gauge_bounds, expected_gauge);
        QCOMPARE(document.cards[1].sparkline_history_seconds, std::optional<std::uint16_t>{90});
        QCOMPARE(document.cards[2].id, std::string{"CDBG_COOLANT_TEMP-1"});
        QCOMPARE(document.cards[2].order, std::uint32_t{2});
        QCOMPARE(document.cards[2].display_type, dashboard::CardDisplayType::Sparkline);
        const std::optional<dashboard::GaugeBoundsOverride> expected_inactive_gauge{{-20.0, 240.0, 5.0}};
        QCOMPARE(document.cards[2].gauge_bounds, expected_inactive_gauge);
        QCOMPARE(document.cards[2].sparkline_history_seconds, std::optional<std::uint16_t>{45});
        QCOMPARE(restored_editor.rowCount(), 3);
        QCOMPARE(restored_presentation.cards()->rowCount(), 3);
        QCOMPARE(restored_connection.canConnect(), true);
    }

    void editorPanelPersistsBesideThePreviewAndReflectsCommittedCommands()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);

        const QList<const char *> required_objects = {
            "dashboardEditorPanel",  "importDashboardButton",   "openDashboardButton", "saveDashboardButton",
            "saveAsDashboardButton", "dashboardDirtyIndicator", "editorCardList",      "addCardButton",
            "removeCardButton",      "moveCardUpButton",        "moveCardDownButton",  "cardChannelCombo",
            "cardConversionCombo",   "cardDisplayTypeCombo",    "gaugeSettings",       "sparklineSettings",
            "unsavedChangesDialog",  "documentErrorBanner",
        };
        for (const char *object_name : required_objects)
        {
            QVERIFY2(application.find(object_name) != nullptr, object_name);
        }

        auto *dashboard_view = qobject_cast<QQuickItem *>(application.find("dashboardView"));
        auto *editor_panel = qobject_cast<QQuickItem *>(application.find("dashboardEditorPanel"));
        QVERIFY(dashboard_view != nullptr);
        QVERIFY(editor_panel != nullptr);
        QCOMPARE(editor_panel->parentItem(), dashboard_view->parentItem());
        QVERIFY(editor_panel->isVisible());

        application.editor.selectCard(QStringLiteral("coolant"));
        QCOMPARE(application.editor.selectedCardId(), QStringLiteral("coolant"));
        QVERIFY(click(application.find("moveCardUpButton")));
        QCOMPARE(application.editor.selectedCardId(), QStringLiteral("coolant"));
        QTRY_COMPARE(
            visual_children_named(dashboard_view, QStringLiteral("cardTitle")).at(0)->property("text").toString(),
            QStringLiteral("Coolant Temperature"));
        QVERIFY(click(application.find("moveCardDownButton")));
        QCOMPARE(application.editor.selectedCardId(), QStringLiteral("coolant"));

        application.editor.setSelectedTitle(QStringLiteral("Coolant Monitor"));
        QTRY_COMPARE(
            visual_children_named(dashboard_view, QStringLiteral("cardTitle")).at(1)->property("text").toString(),
            QStringLiteral("Coolant Monitor"));
        QCOMPARE(application.find("dashboardDirtyIndicator")->property("visible").toBool(), true);
        QVERIFY(click(application.find("saveDashboardButton")));
        QTRY_COMPARE(application.document_controller.isDirty(), false);
        QCOMPARE(application.writer.replace_calls.back().handle, std::string{"editable.ohd"});
    }

    void editorPanelShowsOnlyTheSelectedDisplayTypeSettings()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        application.editor.selectCard(QStringLiteral("rpm"));

        application.editor.setSelectedDisplayType(CardDisplayType::HorizontalGauge);
        QTRY_COMPARE(application.find("gaugeSettings")->property("visible").toBool(), true);
        QCOMPARE(application.find("sparklineSettings")->property("visible").toBool(), false);

        application.editor.setSelectedDisplayType(CardDisplayType::Sparkline);
        QTRY_COMPARE(application.find("gaugeSettings")->property("visible").toBool(), false);
        QCOMPARE(application.find("sparklineSettings")->property("visible").toBool(), true);

        application.editor.setSelectedDisplayType(CardDisplayType::Numeric);
        QTRY_COMPARE(application.find("gaugeSettings")->property("visible").toBool(), false);
        QCOMPARE(application.find("sparklineSettings")->property("visible").toBool(), false);
    }

    void editorPanelCanAddTheFirstCardFromTypedChannelConversions()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        while (application.editor.rowCount() > 0)
        {
            application.editor.removeSelected();
        }

        QObject *channel = application.find("addCardChannelCombo");
        QObject *conversion = application.find("addCardConversionCombo");
        QVERIFY(channel != nullptr);
        QVERIFY(conversion != nullptr);
        QTRY_COMPARE(channel->property("currentValue").toString(), QStringLiteral("CDBG_ENGINE_RPM"));
        QTRY_COMPARE(conversion->property("currentValue").toString(), QStringLiteral("rpm"));
        QVERIFY(click(application.find("addCardButton")));

        QTRY_COMPARE(application.editor.rowCount(), 1);
        QCOMPARE(application.editor.selectedChannelId(), QStringLiteral("CDBG_ENGINE_RPM"));
        QCOMPARE(application.editor.selectedConversionId(), QStringLiteral("rpm"));
    }

    void addConversionChoicesRefreshWhenTheSameChannelIsReplaced()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        while (application.editor.rowCount() > 0)
        {
            application.editor.removeSelected();
        }
        QObject *channel = application.find("addCardChannelCombo");
        QObject *conversion = application.find("addCardConversionCombo");
        QTRY_COMPARE(channel->property("currentValue").toString(), QStringLiteral("CDBG_ENGINE_RPM"));
        QTRY_COMPARE(conversion->property("currentValue").toString(), QStringLiteral("rpm"));

        dashboard::DashboardDocument replacement = mixed_card_document();
        replacement.cards.clear();
        replacement.channels.front().conversions = {
            {.id = "engine-speed-new",
             .expression = "x / 2",
             .unit = "rps",
             .precision = 1,
             .gauge_min = 0.0,
             .gauge_max = 150.0,
             .gauge_step = 10.0},
        };
        auto encoded = dashboard::encode_dashboard_document(replacement);
        QVERIFY2(encoded.has_value(), encoded.has_value() ? "" : encoded.error().detail.c_str());
        application.repository.files["replacement.ohd"] = std::move(*encoded);

        const Status opened = application.document_controller.openDocument("replacement.ohd");

        QVERIFY2(opened.has_value(), opened.has_value() ? "" : opened.error().detail.c_str());
        QTRY_COMPARE(channel->property("currentValue").toString(), QStringLiteral("CDBG_ENGINE_RPM"));
        QTRY_COMPARE(conversion->property("currentValue").toString(), QStringLiteral("engine-speed-new"));
    }

    void runningStateDisablesDocumentAndMutationControlsWithAnExplanation()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        application.editor.setSelectedTitle(QStringLiteral("Blocked while running"));
        application.document_controller.setConnectionState(ConnectionState::Running);
        application.document_controller.requestExit();
        QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), true);

        const QString reason = QStringLiteral("Disconnect to edit the dashboard");
        const QList<const char *> controls = {
            "importDashboardButton",  "openDashboardButton",   "saveDashboardButton",  "saveAsDashboardButton",
            "addCardButton",          "removeCardButton",      "moveCardUpButton",     "moveCardDownButton",
            "cardChannelCombo",       "cardConversionCombo",   "cardDisplayTypeCombo", "addCardChannelCombo",
            "addCardConversionCombo", "cardTitleField",        "gaugeMinimumField",    "gaugeMaximumField",
            "gaugeStepField",         "sparklineHistoryField", "saveUnsavedButton",
        };
        for (const char *object_name : controls)
        {
            QObject *control = application.find(object_name);
            QVERIFY2(control != nullptr, object_name);
            QTRY_COMPARE(control->property("enabled").toBool(), false);
            QCOMPARE(control->property("disabledReason").toString(), reason);
        }
        QCOMPARE(application.find("discardUnsavedButton")->property("enabled").toBool(), true);
        QCOMPARE(application.find("cancelUnsavedButton")->property("enabled").toBool(), true);
    }

    void unsavedDialogButtonsResolveSaveDiscardAndCancelContinuations()
    {
        {
            LoadedApplication application;
            QVERIFY(application.loaded);
            application.editor.setSelectedTitle(QStringLiteral("Save before open"));
            QSignalSpy open_requested(&application.document_controller,
                                      &DashboardDocumentController::openPathRequested);

            application.document_controller.requestOpen();
            QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), true);
            QVERIFY(click(application.find("saveUnsavedButton")));
            QTRY_COMPARE(open_requested.count(), 1);
        }

        {
            LoadedApplication application;
            QVERIFY(application.loaded);
            application.editor.setSelectedTitle(QStringLiteral("Discard before import"));
            QSignalSpy import_requested(&application.document_controller,
                                        &DashboardDocumentController::importPathRequested);

            application.document_controller.requestImport();
            QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), true);
            QVERIFY(click(application.find("discardUnsavedButton")));
            QTRY_COMPARE(import_requested.count(), 1);
        }

        {
            LoadedApplication application;
            QVERIFY(application.loaded);
            application.editor.setSelectedTitle(QStringLiteral("Cancel exit"));
            QSignalSpy exit_approved(&application.document_controller, &DashboardDocumentController::exitApproved);

            application.document_controller.requestExit();
            QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), true);
            QVERIFY(click(application.find("cancelUnsavedButton")));
            QCOMPARE(exit_approved.count(), 0);
            QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), false);
        }
    }

    void closeRequestWaitsForExitApprovalAndCannotReopenThePrompt()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        application.editor.setSelectedTitle(QStringLiteral("Dirty before close"));
        QVERIFY(application.document_controller.isDirty());
        auto *window = qobject_cast<QWindow *>(application.root());
        QVERIFY(window != nullptr);
        QVERIFY(window->isVisible());
        QSignalSpy unsaved_requested(&application.document_controller,
                                     &DashboardDocumentController::unsavedDecisionRequested);
        QSignalSpy exit_approved(&application.document_controller, &DashboardDocumentController::exitApproved);

        static_cast<void>(window->close());

        QTRY_COMPARE(unsaved_requested.count(), 1);
        QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), true);
        QVERIFY(window->isVisible());
        QVERIFY(click(application.find("cancelUnsavedButton")));
        QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), false);
        QCOMPARE(exit_approved.count(), 0);
        QVERIFY(window->isVisible());

        static_cast<void>(window->close());
        QTRY_COMPARE(unsaved_requested.count(), 2);
        QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), true);
        QVERIFY(click(application.find("discardUnsavedButton")));

        QTRY_COMPARE(exit_approved.count(), 1);
        QTRY_VERIFY(!window->isVisible());
        QCOMPARE(unsaved_requested.count(), 2);
    }

    void failedSaveReopensTheUnsavedDialogForEveryRecovery_data()
    {
        QTest::addColumn<QString>("recovery_button");
        QTest::newRow("retry") << QStringLiteral("saveUnsavedButton");
        QTest::newRow("discard") << QStringLiteral("discardUnsavedButton");
        QTest::newRow("cancel") << QStringLiteral("cancelUnsavedButton");
    }

    void failedSaveReopensTheUnsavedDialogForEveryRecovery()
    {
        QFETCH(QString, recovery_button);
        LoadedApplication application;
        QVERIFY(application.loaded);
        application.editor.setSelectedTitle(QStringLiteral("Recover failed save"));
        application.writer.replace_error = Error{ErrorKind::Internal, "disk full"};
        QSignalSpy open_requested(&application.document_controller, &DashboardDocumentController::openPathRequested);

        application.document_controller.requestOpen();
        QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), true);
        QVERIFY(click(application.find("saveUnsavedButton")));

        QTRY_COMPARE(application.find("documentErrorBanner")->property("visible").toBool(), true);
        QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), true);
        QCOMPARE(open_requested.count(), 0);

        application.writer.replace_error.reset();
        QVERIFY(click(application.find(recovery_button.toUtf8().constData())));
        QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), false);
        if (recovery_button == QStringLiteral("saveUnsavedButton"))
        {
            QCOMPARE(application.writer.replace_calls.size(), std::size_t{2});
            QCOMPARE(open_requested.count(), 1);
            QVERIFY(!application.document_controller.isDirty());
        }
        else if (recovery_button == QStringLiteral("discardUnsavedButton"))
        {
            QCOMPARE(application.writer.replace_calls.size(), std::size_t{1});
            QCOMPARE(open_requested.count(), 1);
            QVERIFY(application.document_controller.isDirty());
        }
        else
        {
            QCOMPARE(application.writer.replace_calls.size(), std::size_t{1});
            QCOMPARE(open_requested.count(), 0);
            QVERIFY(application.document_controller.isDirty());
            application.document_controller.requestExit();
            QTRY_COMPARE(application.find("unsavedChangesDialog")->property("visible").toBool(), true);
        }
    }

    void documentErrorsRemainNonDestructiveAndDismissible()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        QObject *banner = application.find("documentErrorBanner");
        QVERIFY(banner != nullptr);
        QCOMPARE(banner->property("visible").toBool(), false);

        application.editor.selectCard(QStringLiteral("manifold-pressure"));
        application.editor.setSelectedGaugeBounds(100.0, 0.0, 0.0);

        QTRY_COMPARE(banner->property("visible").toBool(), true);
        QVERIFY(banner->property("text").toString().contains(QStringLiteral("Edit dashboard")));
        QVERIFY(application.find("dashboardView")->property("visible").toBool());
        QVERIFY(click(application.find("dismissDocumentErrorButton")));
        QCOMPARE(banner->property("visible").toBool(), false);
    }

    void startupWithoutARecentPathKeepsTheImportOpenEmptyState()
    {
        EmptyApplication application;
        QVERIFY(application.loaded);
        QObject *root = application.root();
        QVERIFY(root != nullptr);
        QCOMPARE(root->objectName(), QStringLiteral("desktopQuickRoot"));

        QObject *dashboard_view = application.find("dashboardView");
        QVERIFY(dashboard_view != nullptr);
        auto *dashboard_item = qobject_cast<QQuickItem *>(dashboard_view);
        QVERIFY(dashboard_item != nullptr);
        QCOMPARE(visual_children_named(dashboard_item, QStringLiteral("numericCard")).size(), 0);
        QCOMPARE(visual_children_named(dashboard_item, QStringLiteral("sparklineCard")).size(), 0);
        QCOMPARE(visual_children_named(dashboard_item, QStringLiteral("horizontalGaugeCard")).size(), 0);

        QObject *dashboard_load_error = application.find("loadErrorText");
        QVERIFY(dashboard_load_error != nullptr);
        QCOMPARE(visual_children_named(dashboard_item, QStringLiteral("loadErrorText")).size(), 1);
        QCOMPARE(dashboard_load_error->property("visible").toBool(), false);
        QCOMPARE(application.document_controller.hasDocument(), false);
        QCOMPARE(application.editor.rowCount(), 0);

        QObject *connect_button = application.find("connectButton");
        QVERIFY(connect_button != nullptr);
        QCOMPARE(connect_button->property("enabled").toBool(), false);
        QCOMPARE(application.find("importDashboardButton")->property("enabled").toBool(), true);
        QCOMPARE(application.find("openDashboardButton")->property("enabled").toBool(), true);
        QCOMPARE(application.find("saveDashboardButton")->property("enabled").toBool(), false);
        QCOMPARE(application.find("saveAsDashboardButton")->property("enabled").toBool(), false);
    }

    void failedRecentRestorationDoesNotSubstituteTheBundledDashboard()
    {
        InMemoryFileRepository repository;
        InMemoryAtomicFileWriter writer;
        InMemorySettings settings;
        settings.set(DashboardDocumentController::recentPathKey, "missing.ohd");
        dashboard::DashboardDocumentService documents(repository, writer);
        DashboardDocumentController document_controller(documents, settings);
        DashboardEditorModel editor(document_controller);
        FakePreparationService preparation;
        FakeLoggingEngine logging;
        DashboardConnectionController connection_controller(preparation, logging);
        FakeClock clock;
        DashboardController presentation(std::nullopt, logging, connection_controller, clock);
        QSignalSpy errors(&document_controller, &DashboardDocumentController::errorOccurred);

        const Status restored = document_controller.restoreRecentDocument();

        QVERIFY(!restored.has_value());
        QCOMPARE(errors.count(), 1);
        QCOMPARE(document_controller.hasDocument(), false);
        QCOMPARE(settings.get(DashboardDocumentController::recentPathKey), std::optional<std::string>{"missing.ohd"});
        QQmlApplicationEngine engine;
        QVERIFY(load_root(engine, connection_controller, presentation, document_controller, editor));
        QCOMPARE(presentation.cards()->rowCount(), 0);
        QCOMPARE(presentation.hasLoadError(), false);
        QCOMPARE(connection_controller.canConnect(), false);
        QCOMPARE(editor.rowCount(), 0);
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
        const QList<QObject *> cards = visual_dashboard_cards(dashboard_item);
        QCOMPARE(cards.size(), 3);
        QCOMPARE(cards.at(0)->objectName(), QStringLiteral("numericCard"));
        QCOMPARE(cards.at(1)->objectName(), QStringLiteral("sparklineCard"));
        QCOMPARE(cards.at(2)->objectName(), QStringLiteral("horizontalGaugeCard"));
        for (QObject *card : cards)
        {
            QVERIFY(card->property("usesDashboardCardFrame").isValid());
            QCOMPARE(card->property("usesDashboardCardFrame").toBool(), true);
            auto *card_item = qobject_cast<QQuickItem *>(card);
            QVERIFY(card_item != nullptr);
            QCOMPARE(visual_children_named(card_item, QStringLiteral("cardTitle")).size(), 1);
            QCOMPARE(visual_children_named(card_item, QStringLiteral("cardValue")).size(), 1);
            QCOMPARE(visual_children_named(card_item, QStringLiteral("cardUnit")).size(), 1);
            QCOMPARE(visual_children_named(card_item, QStringLiteral("cardState")).size(), 1);
        }
        QCOMPARE(cards.at(0)->property("implicitHeight").toDouble(),
                 cards.at(1)->property("implicitHeight").toDouble());
        QCOMPARE(cards.at(1)->property("implicitHeight").toDouble(),
                 cards.at(2)->property("implicitHeight").toDouble());
        QVERIFY(cards.at(0)->property("waitingReadingState").isValid());
        QVERIFY(cards.at(0)->property("liveReadingState").isValid());
        QVERIFY(cards.at(0)->property("staleReadingState").isValid());
        QCOMPARE(cards.at(0)->property("waitingReadingState").toInt(), static_cast<int>(ReadingState::Waiting));
        QCOMPARE(cards.at(0)->property("liveReadingState").toInt(), static_cast<int>(ReadingState::Live));
        QCOMPARE(cards.at(0)->property("staleReadingState").toInt(), static_cast<int>(ReadingState::Stale));
        const QList<QObject *> titles = visual_children_named(dashboard_item, QStringLiteral("cardTitle"));
        QCOMPARE(titles.size(), 3);
        QCOMPARE(titles.at(0)->property("text").toString(), QStringLiteral("Tachometer"));
        QCOMPARE(titles.at(1)->property("text").toString(), QStringLiteral("Coolant Temperature"));
        QCOMPARE(titles.at(2)->property("text").toString(), QStringLiteral("Manifold Pressure"));

        const QList<QObject *> values = visual_children_named(dashboard_item, QStringLiteral("cardValue"));
        const QList<QObject *> units = visual_children_named(dashboard_item, QStringLiteral("cardUnit"));
        const QList<QObject *> states = visual_children_named(dashboard_item, QStringLiteral("cardState"));
        const QList<QObject *> ages = visual_children_named(dashboard_item, QStringLiteral("cardAge"));
        QCOMPARE(values.size(), 3);
        QCOMPARE(units.size(), 3);
        QCOMPARE(states.size(), 3);
        QCOMPARE(ages.size(), 3);
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

    void visualizationCardsExposeBoundedGeometryAndRetainStaleReadings()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        QObject *dashboard_view = application.find("dashboardView");
        QVERIFY(dashboard_view != nullptr);
        auto *dashboard_item = qobject_cast<QQuickItem *>(dashboard_view);
        QVERIFY(dashboard_item != nullptr);

        const QList<QObject *> sparkline_cards = visual_children_named(dashboard_item, QStringLiteral("sparklineCard"));
        const QList<QObject *> gauge_cards =
            visual_children_named(dashboard_item, QStringLiteral("horizontalGaugeCard"));
        QCOMPARE(sparkline_cards.size(), 1);
        QCOMPARE(gauge_cards.size(), 1);
        QObject *sparkline = sparkline_cards.front();
        QObject *gauge = gauge_cards.front();
        QCOMPARE(visual_children_named(dashboard_item, QStringLiteral("sparklineCanvas")).size(), 1);
        QCOMPARE(visual_children_named(dashboard_item, QStringLiteral("gaugeCanvas")).size(), 1);
        QCOMPARE(gauge->property("tickCount").toInt(), 12);

        auto *cards_model = static_cast<DashboardCardModel *>(application.presentation.cards());
        cards_model->applySamples({sample("CDBG_MANIFOLD_PRESSURE", 50.0)}, 1000, true);
        QTRY_COMPARE(gauge->property("normalizedValue").toDouble(), 0.5);
        QTRY_COMPARE(gauge->property("overflowDirection").toInt(), 0);

        cards_model->applySamples({sample("CDBG_MANIFOLD_PRESSURE", 125.0)}, 1100, true);
        QTRY_COMPARE(gauge->property("normalizedValue").toDouble(), 1.0);
        QTRY_COMPARE(gauge->property("overflowDirection").toInt(), 1);

        cards_model->applySamples({sample("CDBG_MANIFOLD_PRESSURE", -5.0)}, 1200, true);
        QTRY_COMPARE(gauge->property("normalizedValue").toDouble(), 0.0);
        QTRY_COMPARE(gauge->property("overflowDirection").toInt(), -1);

        cards_model->applySamples({{.sample = sample("CDBG_COOLANT_TEMP", 20.0), .received_at_ms = 1000},
                                   {.sample = sample("CDBG_COOLANT_TEMP", 25.0), .received_at_ms = 1100},
                                   {.sample = sample("CDBG_COOLANT_TEMP", 30.0), .received_at_ms = 1251}},
                                  true);
        QTRY_COMPARE(sparkline->property("segmentCount").toInt(), 2);

        auto *sparkline_item = qobject_cast<QQuickItem *>(sparkline);
        QVERIFY(sparkline_item != nullptr);
        const QList<QObject *> sparkline_values = visual_children_named(sparkline_item, QStringLiteral("cardValue"));
        QCOMPARE(sparkline_values.size(), 1);
        QTRY_COMPARE(sparkline_values.front()->property("text").toString(), QStringLiteral("30.0"));
        QAccessibleInterface *sparkline_accessible = QAccessible::queryAccessibleInterface(sparkline);
        QVERIFY(sparkline_accessible != nullptr);

        cards_model->markReceivedRowsStale();
        QTRY_COMPARE(sparkline->property("cardReadingState").toInt(), static_cast<int>(ReadingState::Stale));
        QCOMPARE(sparkline_values.front()->property("text").toString(), QStringLiteral("30.0"));
        QCOMPARE(sparkline_accessible->text(QAccessible::Name), QString::fromUtf8("Coolant Temperature 30.0 °C Stale"));
    }

    void visualizationGuidesAndOverflowGeometryRespectBounds()
    {
        LoadedApplication application;
        QVERIFY(application.loaded);
        QObject *dashboard_view = application.find("dashboardView");
        QVERIFY(dashboard_view != nullptr);
        auto *dashboard_item = qobject_cast<QQuickItem *>(dashboard_view);
        QVERIFY(dashboard_item != nullptr);

        QObject *sparkline = visual_children_named(dashboard_item, QStringLiteral("sparklineCard")).front();
        QObject *gauge = visual_children_named(dashboard_item, QStringLiteral("horizontalGaugeCard")).front();
        QCOMPARE(gauge->property("tickGuideStrideMultiplier").toInt(), 2);
        QCOMPARE(gauge->property("tickGuideStepValue").toDouble(), 10.0);
        QCOMPARE(sparkline->property("referenceGuideStrideMultiplier").toInt(), 3);
        QCOMPARE(sparkline->property("referenceGuideStepValue").toDouble(), 30.0);

        auto *cards_model = static_cast<DashboardCardModel *>(application.presentation.cards());
        cards_model->applySamples({sample("CDBG_MANIFOLD_PRESSURE", 125.0)}, 1000, true);
        QTRY_COMPARE(gauge->property("overflowTriangleTipOffset").toDouble(), 7.0);
        cards_model->applySamples({sample("CDBG_MANIFOLD_PRESSURE", -5.0)}, 1100, true);
        QTRY_COMPARE(gauge->property("overflowTriangleTipOffset").toDouble(), -7.0);

        const double extreme = std::numeric_limits<double>::max();
        gauge->setProperty("minimumValue", -extreme);
        gauge->setProperty("maximumValue", extreme);
        gauge->setProperty("numericValue", 0.0);
        QCOMPARE(gauge->property("hasFiniteRange").toBool(), false);
        QCOMPARE(gauge->property("range").toDouble(), 0.0);
        QCOMPARE(gauge->property("normalizedValue").toDouble(), 0.0);
        QCOMPARE(gauge->property("overflowDirection").toInt(), 0);
        QCOMPARE(gauge->property("tickCount").toInt(), 0);
        QCOMPARE(gauge->property("tickGuideStepValue").toDouble(), 0.0);

        sparkline->setProperty("minimumValue", -extreme);
        sparkline->setProperty("maximumValue", extreme);
        QCOMPARE(sparkline->property("hasFiniteRange").toBool(), false);
        QCOMPARE(sparkline->property("range").toDouble(), 0.0);
        QCOMPARE(sparkline->property("referenceGuideStepValue").toDouble(), 0.0);
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
        QCOMPARE(titles.size(), 3);
        QCOMPARE(titles.at(0)->property("text").toString(), QStringLiteral("Tachometer"));
        QCOMPARE(titles.at(1)->property("text").toString(), QStringLiteral("Coolant Temperature"));
        QCOMPARE(titles.at(2)->property("text").toString(), QStringLiteral("Manifold Pressure"));
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
        application.editor.selectCard(QStringLiteral("rpm"));
        QVERIFY(application.document_controller.editingEnabled());
        QVERIFY(application.editor.canRemove());
        QSignalSpy availability_changed(&application.editor, &DashboardEditorModel::availabilityChanged);
        application.preparation.next_preparation = prepared_connection();
        QVERIFY(click(application.find("connectButton")));
        QCOMPARE(application.logging.start_calls, 1);
        QCOMPARE(application.find("connectionPanel")->property("transitioning").toBool(), true);
        QCOMPARE(application.find("connectButton")->property("enabled").toBool(), false);
        QCOMPARE(application.find("refreshAdaptersButton")->property("enabled").toBool(), false);
        QCOMPARE(application.document_controller.editingEnabled(), false);
        QCOMPARE(application.editor.canRemove(), false);
        QVERIFY(availability_changed.count() > 0);
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
