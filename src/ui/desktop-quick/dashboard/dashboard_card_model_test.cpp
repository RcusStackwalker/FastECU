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
                 .conversions = {{.id = "temperature", .unit = "°C", .precision = 1}}},
                {.id = "CDBG_ENGINE_RPM",
                 .name = "Engine RPM",
                 .conversions = {{.id = "rpm", .unit = "rpm", .precision = 0}}},
            },
        .cards =
            {
                {.id = "coolant", .channel_id = "CDBG_COOLANT_TEMP", .conversion_id = "temperature", .order = 20},
                {.id = "rpm",
                 .channel_id = "CDBG_ENGINE_RPM",
                 .conversion_id = "rpm",
                 .title = "Tachometer",
                 .order = 10},
            },
    };
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

        QCOMPARE(model.rowCount(), 2);
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
        QCOMPARE(model.roleNames().value(DashboardCardModel::FormattedValueRole), QByteArrayLiteral("formattedValue"));
        QVERIFY(model.containsChannel("CDBG_ENGINE_RPM"));
        QVERIFY(!model.containsChannel("unknown"));

        model.applySamples({sample("unknown", 1.0)}, 100, true);

        QCOMPARE(model.rowCount(), 2);
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
                               DashboardCardModel::LastUpdateAgeTextRole}));

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
};

} // namespace
} // namespace fastecu::desktop_quick

QTEST_MAIN(fastecu::desktop_quick::DashboardCardModelTest)
#include "dashboard_card_model_test.moc"
