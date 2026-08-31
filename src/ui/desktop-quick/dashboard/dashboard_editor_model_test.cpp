#include <QSignalSpy>
#include <QtTest>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/dashboard/dashboard_codec.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_settings.h"
#include "src/ui/desktop-quick/dashboard/dashboard_editor_model.h"

namespace fastecu::desktop_quick
{
namespace
{

dashboard::DashboardConversion conversion(std::string id, std::string unit, double minimum, double maximum, double step)
{
    return dashboard::DashboardConversion{
        .id = std::move(id),
        .expression = "x",
        .unit = std::move(unit),
        .precision = 1,
        .gauge_min = minimum,
        .gauge_max = maximum,
        .gauge_step = step,
    };
}

dashboard::DashboardChannel channel(std::string id, std::string name, std::uint32_t address,
                                    std::vector<dashboard::DashboardConversion> conversions)
{
    return dashboard::DashboardChannel{
        .id = std::move(id),
        .name = std::move(name),
        .description = "Imported channel",
        .address = address,
        .length = 2,
        .raw_assembly = dashboard::RawAssembly::UnsignedIntegerDecimal,
        .conversions = std::move(conversions),
    };
}

dashboard::DashboardCard card(std::string id, std::string channel_id, std::string conversion_id, std::uint32_t order,
                              std::optional<std::string> title = std::nullopt)
{
    return dashboard::DashboardCard{
        .id = std::move(id),
        .channel_id = std::move(channel_id),
        .conversion_id = std::move(conversion_id),
        .display_type = dashboard::CardDisplayType::Numeric,
        .title = std::move(title),
        .order = order,
        .gauge_bounds = std::nullopt,
        .sparkline_history_seconds = std::nullopt,
    };
}

dashboard::DashboardDocument document_with(std::vector<dashboard::DashboardCard> cards)
{
    return dashboard::DashboardDocument{
        .metadata = dashboard::DocumentMetadata{.format_version = 1, .name = "Editable"},
        .connection =
            dashboard::CdbgConnectionProfile{
                .protocol = dashboard::DashboardProtocol::Cdbg,
                .transport = dashboard::DashboardTransport::RawCan,
                .bitrate = 500000,
                .identifier_width = dashboard::CanIdentifierWidth::Standard,
                .request_id = 0x630,
                .reply_id = 0x631,
                .stream_instance = 0,
                .sampling_interval_ms = 50,
                .retry = dashboard::RetryPolicy{100, 3, 3, 250},
                .preferred_adapter = std::nullopt,
            },
        .channels =
            {
                channel("CDBG_ENGINE_RPM", "Engine RPM", 0x100,
                        {conversion("rpm", "rpm", 0.0, 8000.0, 500.0), conversion("krpm", "krpm", 0.0, 8.0, 0.5)}),
                channel("CDBG_COOLANT", "Coolant", 0x102,
                        {conversion("celsius", "C", -40.0, 140.0, 10.0),
                         conversion("fahrenheit", "F", -40.0, 284.0, 20.0)}),
                channel("CDBG_BOOST", "Boost", 0x104,
                        {conversion("bar", "bar", -1.0, 2.0, 0.1), conversion("psi", "psi", -15.0, 30.0, 5.0)}),
            },
        .cards = std::move(cards),
    };
}

QVariant role(const DashboardEditorModel& model, int row, int role)
{
    return model.data(model.index(row), role);
}

QString choice_id(const QVariant& choice)
{
    return choice.toMap().value(QStringLiteral("id")).toString();
}

struct Harness
{
    InMemoryFileRepository repository;
    InMemoryAtomicFileWriter writer;
    InMemorySettings settings;
    dashboard::DashboardDocumentService documents{repository, writer};
    DashboardDocumentController controller{documents, settings};
    DashboardEditorModel editor{controller};

    void open(const dashboard::DashboardDocument& document)
    {
        auto encoded = dashboard::encode_dashboard_document(document);
        QVERIFY2(encoded.has_value(), encoded.error().detail.c_str());
        repository.files["editable.ohd"] = std::move(*encoded);
        const Status opened = controller.openDocument("editable.ohd");
        QVERIFY2(opened.has_value(), opened.error().detail.c_str());
    }
};

} // namespace

class DashboardEditorModelTest : public QObject
{
    Q_OBJECT

  private slots:
    void projectsRolesDocumentOrderChoicesAndAvailability()
    {
        Harness harness;
        harness.open(document_with(
            {card("rpm-card", "CDBG_ENGINE_RPM", "rpm", 0), card("coolant-card", "CDBG_COOLANT", "celsius", 1)}));

        QCOMPARE(harness.editor.roleNames().value(DashboardEditorModel::CardIdRole), QByteArray("cardId"));
        QCOMPARE(harness.editor.roleNames().value(DashboardEditorModel::TitleRole), QByteArray("title"));
        QCOMPARE(harness.editor.roleNames().value(DashboardEditorModel::ChannelNameRole), QByteArray("channelName"));
        QCOMPARE(harness.editor.roleNames().value(DashboardEditorModel::ConversionIdRole), QByteArray("conversionId"));
        QCOMPARE(harness.editor.roleNames().value(DashboardEditorModel::DisplayTypeRole), QByteArray("displayType"));
        QCOMPARE(harness.editor.rowCount(), 2);
        QCOMPARE(role(harness.editor, 0, DashboardEditorModel::CardIdRole).toString(), QStringLiteral("rpm-card"));
        QCOMPARE(role(harness.editor, 0, DashboardEditorModel::TitleRole).toString(), QStringLiteral("Engine RPM"));
        QCOMPARE(role(harness.editor, 1, DashboardEditorModel::ChannelNameRole).toString(), QStringLiteral("Coolant"));
        QCOMPARE(role(harness.editor, 1, DashboardEditorModel::ConversionIdRole).toString(), QStringLiteral("celsius"));
        QCOMPARE(role(harness.editor, 0, DashboardEditorModel::DisplayTypeRole).value<CardDisplayType>(),
                 CardDisplayType::Numeric);

        const QVariantList channels = harness.editor.channelChoices();
        QCOMPARE(channels.size(), 3);
        QCOMPARE(choice_id(channels.at(0)), QStringLiteral("CDBG_ENGINE_RPM"));
        QCOMPARE(choice_id(channels.at(1)), QStringLiteral("CDBG_COOLANT"));
        QCOMPARE(choice_id(channels.at(2)), QStringLiteral("CDBG_BOOST"));
        const QVariantList conversions = harness.editor.conversionChoices();
        QCOMPARE(conversions.size(), 2);
        QCOMPARE(choice_id(conversions.at(0)), QStringLiteral("rpm"));
        QCOMPARE(choice_id(conversions.at(1)), QStringLiteral("krpm"));
        QCOMPARE(harness.editor.displayTypeChoices().size(), 3);

        QCOMPARE(harness.editor.selectedCardId(), QStringLiteral("rpm-card"));
        QVERIFY(harness.editor.canAdd());
        QVERIFY(harness.editor.canRemove());
        QVERIFY(!harness.editor.canMoveUp());
        QVERIFY(harness.editor.canMoveDown());
    }

    void projectsConversionsForAnAddChannelWithoutASelectedCard()
    {
        Harness harness;
        harness.open(document_with({}));

        const QVariantList boost = harness.editor.conversionChoicesForChannel(QStringLiteral("CDBG_BOOST"));
        QCOMPARE(boost.size(), 2);
        QCOMPARE(choice_id(boost.at(0)), QStringLiteral("bar"));
        QCOMPARE(choice_id(boost.at(1)), QStringLiteral("psi"));
        QCOMPARE(harness.editor.conversionChoicesForChannel(QStringLiteral("missing")), QVariantList{});
        QCOMPARE(harness.editor.selectedCardId(), QString{});
        QCOMPARE(harness.editor.conversionChoices(), QVariantList{});
    }

    void addsConfiguresAndMovesACardTransactionally()
    {
        Harness harness;
        harness.open(document_with({card("coolant-card", "CDBG_COOLANT", "celsius", 0)}));

        harness.editor.addCard(QStringLiteral("CDBG_ENGINE_RPM"), QStringLiteral("rpm"));
        QCOMPARE(harness.editor.selectedCardId(), QStringLiteral("CDBG_ENGINE_RPM-1"));
        harness.editor.setSelectedTitle(QStringLiteral("Tachometer"));
        harness.editor.setSelectedDisplayType(CardDisplayType::Sparkline);
        harness.editor.setSelectedSparklineHistorySeconds(30);
        harness.editor.moveSelectedUp();

        const auto& cards = harness.controller.document()->cards;
        QCOMPARE(cards.size(), 2U);
        QCOMPARE(cards.at(0).id, std::string("CDBG_ENGINE_RPM-1"));
        QCOMPARE(cards.at(0).title, std::optional<std::string>{"Tachometer"});
        QCOMPARE(cards.at(0).display_type, dashboard::CardDisplayType::Sparkline);
        QCOMPARE(cards.at(0).sparkline_history_seconds, std::optional<std::uint16_t>{30});
        QCOMPARE(cards.at(0).order, 0U);
        QCOMPARE(cards.at(1).id, std::string("coolant-card"));
        QCOMPARE(cards.at(1).order, 1U);
        QCOMPARE(harness.editor.selectedCardId(), QStringLiteral("CDBG_ENGINE_RPM-1"));
        QVERIFY(harness.controller.isDirty());
    }

    void removalSelectsTheNextCardOrThePreviousCardAtTheEnd()
    {
        Harness harness;
        harness.open(document_with({card("rpm-card", "CDBG_ENGINE_RPM", "rpm", 0),
                                    card("coolant-card", "CDBG_COOLANT", "celsius", 1),
                                    card("boost-card", "CDBG_BOOST", "bar", 2)}));

        harness.editor.selectCard(QStringLiteral("coolant-card"));
        harness.editor.removeSelected();
        QCOMPARE(harness.editor.selectedCardId(), QStringLiteral("boost-card"));
        QCOMPARE(harness.controller.document()->cards.at(0).order, 0U);
        QCOMPARE(harness.controller.document()->cards.at(1).order, 1U);

        harness.editor.removeSelected();
        QCOMPARE(harness.editor.selectedCardId(), QStringLiteral("rpm-card"));
        QCOMPARE(harness.editor.rowCount(), 1);
        harness.editor.removeSelected();
        QCOMPARE(harness.editor.selectedCardId(), QString{});
        QVERIFY(!harness.editor.canRemove());
    }

    void editsChoicesAndTypeSpecificConfigurationWithRollback()
    {
        Harness harness;
        harness.open(document_with({card("rpm-card", "CDBG_ENGINE_RPM", "rpm", 0)}));

        harness.editor.setSelectedChannel(QStringLiteral("CDBG_BOOST"));
        QCOMPARE(harness.editor.selectedChannelId(), QStringLiteral("CDBG_BOOST"));
        QCOMPARE(harness.editor.selectedConversionId(), QStringLiteral("bar"));
        QCOMPARE(choice_id(harness.editor.conversionChoices().at(0)), QStringLiteral("bar"));
        harness.editor.setSelectedConversion(QStringLiteral("psi"));
        QCOMPARE(harness.editor.selectedConversionId(), QStringLiteral("psi"));

        harness.editor.setSelectedDisplayType(CardDisplayType::HorizontalGauge);
        QCOMPARE(harness.editor.selectedGaugeMinimum(), -15.0);
        QCOMPARE(harness.editor.selectedGaugeMaximum(), 30.0);
        QCOMPARE(harness.editor.selectedGaugeStep(), 5.0);
        harness.editor.setSelectedGaugeBounds(-10.0, 25.0, 2.5);
        const dashboard::DashboardDocument valid_gauge = *harness.controller.document();
        const std::optional<dashboard::GaugeBoundsOverride> expected_bounds{{-10.0, 25.0, 2.5}};
        QCOMPARE(valid_gauge.cards.front().gauge_bounds, expected_bounds);

        QSignalSpy errors(&harness.controller, &DashboardDocumentController::errorOccurred);
        harness.editor.setSelectedGaugeBounds(25.0, -10.0, 0.0);
        QCOMPARE(*harness.controller.document(), valid_gauge);
        QCOMPARE(errors.count(), 1);

        harness.editor.setSelectedDisplayType(CardDisplayType::Sparkline);
        harness.editor.setSelectedSparklineHistorySeconds(30);
        const dashboard::DashboardDocument valid_sparkline = *harness.controller.document();
        QCOMPARE(valid_sparkline.cards.front().sparkline_history_seconds, std::optional<std::uint16_t>{30});
        QVERIFY(!valid_sparkline.cards.front().gauge_bounds.has_value());
        harness.editor.setSelectedSparklineHistorySeconds(301);
        QCOMPARE(*harness.controller.document(), valid_sparkline);
        QCOMPARE(errors.count(), 2);
    }

    void preventsDuplicateChannelsAndUsesTheFirstUnusedStableIdSuffix()
    {
        Harness harness;
        harness.open(document_with({card("CDBG_ENGINE_RPM-1", "CDBG_BOOST", "bar", 0)}));

        harness.editor.addCard(QStringLiteral("CDBG_ENGINE_RPM"), QStringLiteral("rpm"));
        QCOMPARE(harness.editor.selectedCardId(), QStringLiteral("CDBG_ENGINE_RPM-2"));
        const dashboard::DashboardDocument before_duplicate = *harness.controller.document();
        QSignalSpy errors(&harness.controller, &DashboardDocumentController::errorOccurred);

        harness.editor.addCard(QStringLiteral("CDBG_BOOST"), QStringLiteral("bar"));

        QCOMPARE(*harness.controller.document(), before_duplicate);
        QCOMPARE(harness.editor.selectedCardId(), QStringLiteral("CDBG_ENGINE_RPM-2"));
        QCOMPARE(errors.count(), 1);
    }

    void semanticNoOpsDoNotDirtyOrCommitTheDocument()
    {
        Harness harness;
        harness.open(
            document_with({card("rpm-card", "CDBG_ENGINE_RPM", "rpm", 0, std::optional<std::string>{"Tachometer"})}));
        QSignalSpy committed(&harness.controller, &DashboardDocumentController::documentCommitted);

        harness.editor.selectCard(QStringLiteral("rpm-card"));
        harness.editor.setSelectedChannel(QStringLiteral("CDBG_ENGINE_RPM"));
        harness.editor.setSelectedConversion(QStringLiteral("rpm"));
        harness.editor.setSelectedTitle(QStringLiteral("Tachometer"));
        harness.editor.setSelectedDisplayType(CardDisplayType::Numeric);
        harness.editor.moveSelectedUp();
        harness.editor.moveSelectedDown();

        QVERIFY(!harness.controller.isDirty());
        QCOMPARE(committed.count(), 0);
    }

    void projectedFallbackTitleRoundTripsAsASemanticNoOp()
    {
        Harness harness;
        harness.open(document_with({card("rpm-card", "CDBG_ENGINE_RPM", "rpm", 0)}));
        QSignalSpy committed(&harness.controller, &DashboardDocumentController::documentCommitted);

        QCOMPARE(harness.editor.selectedTitle(), QStringLiteral("Engine RPM"));
        harness.editor.setSelectedTitle(harness.editor.selectedTitle());

        QVERIFY(!harness.controller.document()->cards.front().title.has_value());
        QVERIFY(!harness.controller.isDirty());
        QCOMPARE(committed.count(), 0);
    }

    void xmlInvalidTitleIsRejectedTransactionally()
    {
        Harness harness;
        harness.open(document_with({card("rpm-card", "CDBG_ENGINE_RPM", "rpm", 0)}));
        const dashboard::DashboardDocument before = *harness.controller.document();
        QSignalSpy errors(&harness.controller, &DashboardDocumentController::errorOccurred);
        QString invalid_title = QStringLiteral("Bad");
        invalid_title.append(QChar{0x1});

        harness.editor.setSelectedTitle(invalid_title);

        QCOMPARE(*harness.controller.document(), before);
        QVERIFY(!harness.controller.isDirty());
        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors.at(0).at(0).toString(), QStringLiteral("Edit dashboard"));
        QCOMPARE(errors.at(0).at(1).toString(),
                 QStringLiteral("cards[rpm-card].title: must contain only valid UTF-8 XML 1.0 characters"));
        QCOMPARE(errors.at(0).at(2).value<ErrorKind>(), ErrorKind::InvalidConfig);
    }

    void outOfRangeDisplayTypeIsRejectedTransactionally()
    {
        Harness harness;
        harness.open(document_with({card("rpm-card", "CDBG_ENGINE_RPM", "rpm", 0)}));
        const dashboard::DashboardDocument before = *harness.controller.document();
        QSignalSpy errors(&harness.controller, &DashboardDocumentController::errorOccurred);

        const CardDisplayType invalid = static_cast<CardDisplayType>(99);
        QVERIFY(QMetaObject::invokeMethod(&harness.editor, "setSelectedDisplayType", Qt::DirectConnection,
                                          Q_ARG(CardDisplayType, invalid)));

        QCOMPARE(*harness.controller.document(), before);
        QVERIFY(!harness.controller.isDirty());
        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors.at(0).at(0).toString(), QStringLiteral("Edit dashboard"));
        QCOMPARE(errors.at(0).at(1).toString(), QStringLiteral("cards[rpm-card].display-type: is not supported"));
        QCOMPARE(errors.at(0).at(2).value<ErrorKind>(), ErrorKind::InvalidConfig);
    }

    void connectedStateRejectsEveryEditorOperation()
    {
        Harness harness;
        harness.open(document_with(
            {card("rpm-card", "CDBG_ENGINE_RPM", "rpm", 0), card("coolant-card", "CDBG_COOLANT", "celsius", 1)}));
        const dashboard::DashboardDocument before = *harness.controller.document();
        const QString selected_before = harness.editor.selectedCardId();
        harness.controller.setConnectionState(ConnectionState::Running);
        QSignalSpy errors(&harness.controller, &DashboardDocumentController::errorOccurred);

        QVERIFY(!harness.editor.canAdd());
        QVERIFY(!harness.editor.canRemove());
        QVERIFY(!harness.editor.canMoveUp());
        QVERIFY(!harness.editor.canMoveDown());
        harness.editor.selectCard(QStringLiteral("coolant-card"));
        harness.editor.addCard(QStringLiteral("CDBG_BOOST"), QStringLiteral("bar"));
        harness.editor.removeSelected();
        harness.editor.moveSelectedUp();
        harness.editor.moveSelectedDown();
        harness.editor.setSelectedChannel(QStringLiteral("CDBG_BOOST"));
        harness.editor.setSelectedConversion(QStringLiteral("krpm"));
        harness.editor.setSelectedTitle(QStringLiteral("Blocked"));
        harness.editor.setSelectedDisplayType(CardDisplayType::Sparkline);
        harness.editor.setSelectedGaugeBounds(0.0, 1.0, 0.1);
        harness.editor.setSelectedSparklineHistorySeconds(30);

        QCOMPARE(*harness.controller.document(), before);
        QCOMPARE(harness.editor.selectedCardId(), selected_before);
        QCOMPARE(errors.count(), 11);
    }
};

} // namespace fastecu::desktop_quick

QTEST_GUILESS_MAIN(fastecu::desktop_quick::DashboardEditorModelTest)

#include "dashboard_editor_model_test.moc"
