#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/logging/logging_types.h"

namespace fastecu::desktop_quick
{

struct ReceivedLogSample
{
    logging::LogSample sample;
    std::uint64_t received_at_ms;
};

class DashboardCardModel final : public QAbstractListModel
{
    Q_OBJECT

  public:
    enum class ReadingState
    {
        Waiting,
        Live,
        Stale,
    };
    Q_ENUM(ReadingState)

    enum class CardDisplayType
    {
        Numeric,
        Sparkline,
        HorizontalGauge,
    };
    Q_ENUM(CardDisplayType)

    enum Role
    {
        CardIdRole = Qt::UserRole + 1,
        ChannelIdRole,
        TitleRole,
        FormattedValueRole,
        NumericValueRole,
        UnitRole,
        PrecisionRole,
        ReadingStateRole,
        HasReadingRole,
        LastUpdateAgeTextRole,
        DisplayTypeRole,
        MinimumValueRole,
        MaximumValueRole,
        StepValueRole,
        SparklineHistorySecondsRole,
        SparklinePointsRole,
    };
    Q_ENUM(Role)

    explicit DashboardCardModel(const dashboard::DashboardDocument& document, QObject *parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void applySamples(const QVector<logging::LogSample>& samples, std::uint64_t now_ms, bool running);
    void applySamples(const QVector<ReceivedLogSample>& samples, bool running);
    void clearSparklineHistories();
    void markReceivedRowsStale();
    void updateAges(std::uint64_t now_ms);
    bool hasReceivedRows() const;
    bool containsChannel(std::string_view channel_id) const;

  private:
    struct SparklinePoint
    {
        std::uint64_t timestamp_ms;
        double value;
        bool starts_segment;
    };

    struct Row
    {
        QString card_id;
        QString channel_id;
        QString title;
        QString unit;
        int precision = 0;
        CardDisplayType display_type = CardDisplayType::Numeric;
        double minimum_value = 0.0;
        double maximum_value = 1.0;
        double step_value = 1.0;
        int sparkline_history_seconds = 0;
        QVector<SparklinePoint> sparkline_points;
        std::uint64_t gap_threshold_ms = 100;
        double numeric_value = 0.0;
        std::uint64_t last_update_ms = 0;
        std::uint64_t age_seconds = 0;
        ReadingState reading_state = ReadingState::Waiting;
        bool has_reading = false;
    };

    static CardDisplayType displayTypeFor(dashboard::CardDisplayType display_type);
    void notifyChangedRows(const QVector<QVector<int>>& roles_by_row);
    QString ageText(const Row& row) const;

    QVector<Row> rows_;
    std::unordered_map<std::string, int> rows_by_channel_;
};

using ReadingState = DashboardCardModel::ReadingState;
using CardDisplayType = DashboardCardModel::CardDisplayType;

} // namespace fastecu::desktop_quick

Q_DECLARE_METATYPE(fastecu::desktop_quick::ReadingState)
Q_DECLARE_METATYPE(fastecu::desktop_quick::CardDisplayType)
