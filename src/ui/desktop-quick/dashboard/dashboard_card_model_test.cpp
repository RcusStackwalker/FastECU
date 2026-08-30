#include <QSignalSpy>
#include <QtTest>

#include <limits>

#include "src/ui/desktop-quick/dashboard/dashboard_card_model.h"

namespace fastecu::desktop_quick
{
namespace
{

logging::LogSample sample(const char *channel_id, double numeric_value)
{
    return {.channel_id = channel_id, .numeric_value = numeric_value};
}

dashboard::DashboardDocument two_card_document()
{
    return {
        .channels =
            {
                {.id = "CDBG_COOLANT_TEMP",
                 .name = "Coolant Temperature",
                 .conversions = {{.id = "temperature",
                                  .unit = "°C",
                                  .precision = 1,
                                  .gauge_min = -40.0,
                                  .gauge_max = 260.0,
                                  .gauge_step = 10.0}}},
                {.id = "CDBG_ENGINE_RPM",
                 .name = "Engine RPM",
                 .conversions = {{.id = "rpm", .unit = "rpm", .precision = 0}}},
                {.id = "CDBG_MANIFOLD_PRESSURE",
                 .name = "Manifold Pressure",
                 .conversions = {{.id = "pressure", .unit = "kPa", .precision = 0}}},
            },
        .cards =
            {
                {.id = "coolant",
                 .channel_id = "CDBG_COOLANT_TEMP",
                 .conversion_id = "temperature",
                 .display_type = dashboard::CardDisplayType::Sparkline,
                 .order = 20,
                 .sparkline_history_seconds = 30},
                {.id = "rpm",
                 .channel_id = "CDBG_ENGINE_RPM",
                 .conversion_id = "rpm",
                 .display_type = dashboard::CardDisplayType::Numeric,
                 .title = "Tachometer",
                 .order = 10},
                {.id = "manifold-pressure",
                 .channel_id = "CDBG_MANIFOLD_PRESSURE",
                 .conversion_id = "pressure",
                 .display_type = dashboard::CardDisplayType::HorizontalGauge,
                 .order = 30,
                 .gauge_bounds = dashboard::GaugeBoundsOverride{0.0, 9000.0, 250.0}},
            },
    };
}

dashboard::DashboardDocument sparkline_document(int history_seconds = 2, std::uint32_t sampling_interval_ms = 50)
{
    dashboard::DashboardDocument document = two_card_document();
    document.connection.sampling_interval_ms = sampling_interval_ms;
    document.cards.at(0).sparkline_history_seconds = history_seconds;
    return document;
}

QVariant role(const DashboardCardModel& model, int row, DashboardCardModel::Role role)
{
    return model.data(model.index(row, 0), role);
}

ReadingState state(const DashboardCardModel& model, int row)
{
    return role(model, row, DashboardCardModel::ReadingStateRole).value<ReadingState>();
}

class DashboardCardModelTest final : public QObject
{
    Q_OBJECT

  private slots:
    void projectsConfiguredCardsInDisplayOrderWithoutChangingRows()
    {
        DashboardCardModel model(two_card_document());
        QSignalSpy reset_spy(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted_spy(&model, &QAbstractItemModel::rowsInserted);

        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(role(model, 0, DashboardCardModel::CardIdRole), QStringLiteral("rpm"));
        QCOMPARE(role(model, 0, DashboardCardModel::ChannelIdRole), QStringLiteral("CDBG_ENGINE_RPM"));
        QCOMPARE(role(model, 0, DashboardCardModel::TitleRole), QStringLiteral("Tachometer"));
        QCOMPARE(role(model, 0, DashboardCardModel::UnitRole), QStringLiteral("rpm"));
        QCOMPARE(role(model, 0, DashboardCardModel::PrecisionRole), 0);
        QCOMPARE(role(model, 0, DashboardCardModel::FormattedValueRole), QStringLiteral("—"));
        QCOMPARE(role(model, 0, DashboardCardModel::HasReadingRole), false);
        QCOMPARE(state(model, 0), ReadingState::Waiting);
        QCOMPARE(role(model, 1, DashboardCardModel::CardIdRole), QStringLiteral("coolant"));
        QCOMPARE(role(model, 1, DashboardCardModel::TitleRole), QStringLiteral("Coolant Temperature"));
        QCOMPARE(role(model, 1, DashboardCardModel::UnitRole), QString::fromUtf8("°C"));
        QCOMPARE(role(model, 1, DashboardCardModel::PrecisionRole), 1);
        QCOMPARE(role(model, 0, DashboardCardModel::DisplayTypeRole).value<CardDisplayType>(),
                 CardDisplayType::Numeric);
        QCOMPARE(role(model, 1, DashboardCardModel::DisplayTypeRole).value<CardDisplayType>(),
                 CardDisplayType::Sparkline);
        QCOMPARE(role(model, 2, DashboardCardModel::DisplayTypeRole).value<CardDisplayType>(),
                 CardDisplayType::HorizontalGauge);
        QCOMPARE(role(model, 1, DashboardCardModel::MinimumValueRole), -40.0);
        QCOMPARE(role(model, 1, DashboardCardModel::MaximumValueRole), 260.0);
        QCOMPARE(role(model, 1, DashboardCardModel::StepValueRole), 10.0);
        QCOMPARE(role(model, 1, DashboardCardModel::SparklineHistorySecondsRole), 30);
        QCOMPARE(role(model, 2, DashboardCardModel::MinimumValueRole), 0.0);
        QCOMPARE(role(model, 2, DashboardCardModel::MaximumValueRole), 9000.0);
        QCOMPARE(role(model, 2, DashboardCardModel::StepValueRole), 250.0);
        QCOMPARE(role(model, 0, DashboardCardModel::SparklineHistorySecondsRole), 0);
        QCOMPARE(role(model, 2, DashboardCardModel::SparklineHistorySecondsRole), 0);
        QCOMPARE(model.roleNames().value(DashboardCardModel::FormattedValueRole), QByteArrayLiteral("formattedValue"));
        QCOMPARE(model.roleNames().value(DashboardCardModel::DisplayTypeRole), QByteArrayLiteral("displayType"));
        QVERIFY(model.containsChannel("CDBG_ENGINE_RPM"));
        QVERIFY(!model.containsChannel("unknown"));

        model.applySamples({sample("unknown", 1.0)}, 100, true);

        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(reset_spy.count(), 0);
        QCOMPARE(inserted_spy.count(), 0);
    }

    void updatesOnlyReceivedRowsAndFormatsWithConfiguredPrecision()
    {
        DashboardCardModel model(two_card_document());

        model.applySamples({sample("CDBG_ENGINE_RPM", 3125.4)}, 1000, true);

        QCOMPARE(role(model, 0, DashboardCardModel::FormattedValueRole), QStringLiteral("3125"));
        QCOMPARE(role(model, 0, DashboardCardModel::NumericValueRole), 3125.4);
        QCOMPARE(role(model, 0, DashboardCardModel::HasReadingRole), true);
        QCOMPARE(state(model, 0), ReadingState::Live);
        QCOMPARE(role(model, 1, DashboardCardModel::FormattedValueRole), QStringLiteral("—"));
        QCOMPARE(state(model, 1), ReadingState::Waiting);

        model.applySamples({sample("CDBG_COOLANT_TEMP", 88.25)}, 2000, true);

        QCOMPARE(role(model, 0, DashboardCardModel::FormattedValueRole), QStringLiteral("3125"));
        QCOMPARE(role(model, 1, DashboardCardModel::FormattedValueRole), QStringLiteral("88.3"));
        QCOMPARE(state(model, 1), ReadingState::Live);
    }

    void marksReceivedReadingsStaleAndResumesWhenRunningSamplesArrive()
    {
        DashboardCardModel model(two_card_document());
        model.applySamples({sample("CDBG_ENGINE_RPM", 3125.4)}, 1000, true);

        model.markReceivedRowsStale();

        QCOMPARE(state(model, 0), ReadingState::Stale);
        QCOMPARE(state(model, 1), ReadingState::Waiting);

        model.applySamples({sample("CDBG_ENGINE_RPM", 3000.0)}, 2000, true);

        QCOMPARE(role(model, 0, DashboardCardModel::FormattedValueRole), QStringLiteral("3000"));
        QCOMPARE(state(model, 0), ReadingState::Live);
    }

    void preservesReceivedValueButMarksItStaleWhenSamplesArriveOutsideRunningState()
    {
        DashboardCardModel model(two_card_document());

        model.applySamples({sample("CDBG_ENGINE_RPM", 3125.4)}, 1000, false);

        QCOMPARE(role(model, 0, DashboardCardModel::FormattedValueRole), QStringLiteral("3125"));
        QCOMPARE(state(model, 0), ReadingState::Stale);
    }

    void rejectsUnknownAndNonFiniteSamplesWithoutMutatingRows()
    {
        DashboardCardModel model(two_card_document());
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);

        model.applySamples({sample("unknown", 1.0), sample("CDBG_ENGINE_RPM", std::numeric_limits<double>::infinity()),
                            sample("CDBG_COOLANT_TEMP", std::numeric_limits<double>::quiet_NaN())},
                           1000, true);

        QCOMPARE(changes.count(), 0);
        QCOMPARE(state(model, 0), ReadingState::Waiting);
        QCOMPARE(state(model, 1), ReadingState::Waiting);
        QVERIFY(!model.hasReceivedRows());
    }

    void reportsMonotonicAgesAndEmitsCompactChangedRanges()
    {
        DashboardCardModel model(two_card_document());
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        model.applySamples({sample("CDBG_ENGINE_RPM", 3125.4)}, 1000, true);

        QCOMPARE(changes.count(), 1);
        const auto update = changes.takeFirst();
        QCOMPARE(update.at(0).value<QModelIndex>().row(), 0);
        QCOMPARE(update.at(1).value<QModelIndex>().row(), 0);
        QCOMPARE(update.at(2).value<QVector<int>>(),
                 QVector<int>({DashboardCardModel::FormattedValueRole, DashboardCardModel::NumericValueRole,
                               DashboardCardModel::ReadingStateRole, DashboardCardModel::HasReadingRole,
                               DashboardCardModel::LastUpdateAgeTextRole}));

        model.updateAges(13000);

        QCOMPARE(role(model, 0, DashboardCardModel::LastUpdateAgeTextRole), QStringLiteral("Last update 12s ago"));
        QCOMPARE(changes.count(), 1);
        const auto age_update = changes.takeFirst();
        QCOMPARE(age_update.at(0).value<QModelIndex>().row(), 0);
        QCOMPARE(age_update.at(1).value<QModelIndex>().row(), 0);
        QCOMPARE(age_update.at(2).value<QVector<int>>(), QVector<int>({DashboardCardModel::LastUpdateAgeTextRole}));

        model.updateAges(500);

        QCOMPARE(role(model, 0, DashboardCardModel::LastUpdateAgeTextRole), QStringLiteral("Last update 12s ago"));
        QCOMPARE(changes.count(), 0);
    }

    void coalescesAdjacentRowsForBatchStaleAndAgeNotifications()
    {
        DashboardCardModel model(two_card_document());
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);

        model.applySamples({sample("CDBG_ENGINE_RPM", 3125.4), sample("CDBG_COOLANT_TEMP", 88.25)}, 1000, true);

        QCOMPARE(changes.count(), 1);
        auto update = changes.takeFirst();
        QCOMPARE(update.at(0).value<QModelIndex>().row(), 0);
        QCOMPARE(update.at(1).value<QModelIndex>().row(), 1);
        QCOMPARE(update.at(2).value<QVector<int>>(),
                 QVector<int>({DashboardCardModel::FormattedValueRole, DashboardCardModel::NumericValueRole,
                               DashboardCardModel::ReadingStateRole, DashboardCardModel::HasReadingRole,
                               DashboardCardModel::LastUpdateAgeTextRole, DashboardCardModel::SparklinePointsRole}));

        model.markReceivedRowsStale();

        QCOMPARE(changes.count(), 1);
        update = changes.takeFirst();
        QCOMPARE(update.at(0).value<QModelIndex>().row(), 0);
        QCOMPARE(update.at(1).value<QModelIndex>().row(), 1);
        QCOMPARE(update.at(2).value<QVector<int>>(), QVector<int>({DashboardCardModel::ReadingStateRole}));

        model.updateAges(13000);

        QCOMPARE(changes.count(), 1);
        update = changes.takeFirst();
        QCOMPARE(update.at(0).value<QModelIndex>().row(), 0);
        QCOMPARE(update.at(1).value<QModelIndex>().row(), 1);
        QCOMPARE(update.at(2).value<QVector<int>>(), QVector<int>({DashboardCardModel::LastUpdateAgeTextRole}));
    }

    void retainsOnlyBoundedSparklineHistoryAndSerializesPointsRelativeToNewest()
    {
        DashboardCardModel model(sparkline_document());

        model.applySamples({ReceivedLogSample{sample("CDBG_COOLANT_TEMP", 10.0), 1000}}, true);
        model.applySamples({ReceivedLogSample{sample("CDBG_COOLANT_TEMP", 20.0), 1500}}, true);
        model.applySamples({ReceivedLogSample{sample("CDBG_COOLANT_TEMP", 30.0), 3101}}, true);
        model.applySamples({ReceivedLogSample{sample("CDBG_ENGINE_RPM", 2000.0), 3101}}, true);

        const QVariantList points = role(model, 1, DashboardCardModel::SparklinePointsRole).toList();
        QCOMPARE(points.size(), 2);
        QCOMPARE(points.at(0).toMap().value("elapsedMs").toLongLong(), -1601);
        QCOMPARE(points.at(0).toMap().value("value").toDouble(), 20.0);
        QCOMPARE(points.at(0).toMap().value("startsSegment").toBool(), true);
        QCOMPARE(points.at(1).toMap().value("elapsedMs").toLongLong(), 0);
        QCOMPARE(points.at(1).toMap().value("value").toDouble(), 30.0);
        QCOMPARE(role(model, 0, DashboardCardModel::SparklinePointsRole).toList().size(), 0);
        QCOMPARE(model.roleNames().value(DashboardCardModel::SparklinePointsRole),
                 QByteArrayLiteral("sparklinePoints"));
    }

    void replacesSameTimestampAndNotifiesSparklineRoleOnlyForSparklineRows()
    {
        DashboardCardModel model(sparkline_document());
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);

        model.applySamples({ReceivedLogSample{sample("CDBG_ENGINE_RPM", 2000.0), 1000}}, true);

        QCOMPARE(changes.count(), 1);
        QVERIFY(!changes.takeFirst().at(2).value<QVector<int>>().contains(DashboardCardModel::SparklinePointsRole));

        model.applySamples({ReceivedLogSample{sample("CDBG_COOLANT_TEMP", 10.0), 1000}}, true);
        model.applySamples({ReceivedLogSample{sample("CDBG_COOLANT_TEMP", 20.0), 1000}}, true);

        QCOMPARE(changes.count(), 2);
        for (const QList<QVariant>& notification : changes)
        {
            QCOMPARE(notification.at(0).value<QModelIndex>().row(), 1);
            QVERIFY(notification.at(2).value<QVector<int>>().contains(DashboardCardModel::SparklinePointsRole));
        }
        const QVariantList points = role(model, 1, DashboardCardModel::SparklinePointsRole).toList();
        QCOMPARE(points.size(), 1);
        QCOMPARE(points.at(0).toMap().value("elapsedMs").toLongLong(), 0);
        QCOMPARE(points.at(0).toMap().value("value").toDouble(), 20.0);
        QCOMPARE(points.at(0).toMap().value("startsSegment").toBool(), true);
    }

    void startsSegmentsAfterStrictGapThresholdAndClearsOnlyExistingHistories()
    {
        DashboardCardModel model(sparkline_document());
        model.applySamples({ReceivedLogSample{sample("CDBG_COOLANT_TEMP", 10.0), 1000}}, true);
        model.applySamples({ReceivedLogSample{sample("CDBG_COOLANT_TEMP", 20.0), 1100}}, true);
        model.applySamples({ReceivedLogSample{sample("CDBG_COOLANT_TEMP", 30.0), 1251}}, true);
        const QVariantList points = role(model, 1, DashboardCardModel::SparklinePointsRole).toList();

        QCOMPARE(points.size(), 3);
        QCOMPARE(points.at(0).toMap().value("startsSegment").toBool(), true);
        QCOMPARE(points.at(1).toMap().value("startsSegment").toBool(), false);
        QCOMPARE(points.at(2).toMap().value("startsSegment").toBool(), true);
        QCOMPARE(role(model, 1, DashboardCardModel::FormattedValueRole), QStringLiteral("30.0"));
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);

        model.clearSparklineHistories();

        QCOMPARE(role(model, 1, DashboardCardModel::SparklinePointsRole).toList().size(), 0);
        QCOMPARE(role(model, 1, DashboardCardModel::FormattedValueRole), QStringLiteral("30.0"));
        QCOMPARE(changes.count(), 1);
        const QList<QVariant> notification = changes.takeFirst();
        QCOMPARE(notification.at(0).value<QModelIndex>().row(), 1);
        QCOMPARE(notification.at(1).value<QModelIndex>().row(), 1);
        QCOMPARE(notification.at(2).value<QVector<int>>(), QVector<int>({DashboardCardModel::SparklinePointsRole}));
    }
};

} // namespace
} // namespace fastecu::desktop_quick

QTEST_MAIN(fastecu::desktop_quick::DashboardCardModelTest)
#include "dashboard_card_model_test.moc"
