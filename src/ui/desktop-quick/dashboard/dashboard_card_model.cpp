#include "src/ui/desktop-quick/dashboard/dashboard_card_model.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace fastecu::desktop_quick
{
namespace
{

struct ResolvedCard
{
    const dashboard::DashboardCard *card;
    const dashboard::DashboardChannel *channel;
    const dashboard::DashboardConversion *conversion;
};

} // namespace

DashboardCardModel::DashboardCardModel(const dashboard::DashboardDocument& document, QObject *parent)
    : QAbstractListModel(parent)
{
    std::unordered_map<std::string_view, const dashboard::DashboardChannel *> channels;
    std::unordered_map<std::string, const dashboard::DashboardConversion *> conversions;
    for (const dashboard::DashboardChannel& channel : document.channels)
    {
        channels.emplace(channel.id, &channel);
        for (const dashboard::DashboardConversion& conversion : channel.conversions)
        {
            conversions.emplace(channel.id + "\x1f" + conversion.id, &conversion);
        }
    }

    std::vector<ResolvedCard> cards;
    cards.reserve(document.cards.size());
    for (const dashboard::DashboardCard& card : document.cards)
    {
        const dashboard::DashboardChannel& channel = *channels.at(card.channel_id);
        const dashboard::DashboardConversion& conversion =
            *conversions.at(card.channel_id + "\x1f" + card.conversion_id);
        cards.push_back({.card = &card, .channel = &channel, .conversion = &conversion});
    }
    std::stable_sort(cards.begin(), cards.end(), [](const ResolvedCard& left, const ResolvedCard& right)
                     { return left.card->order < right.card->order; });

    rows_.reserve(static_cast<qsizetype>(cards.size()));
    for (const ResolvedCard& resolved : cards)
    {
        const dashboard::DashboardCard& card = *resolved.card;
        const dashboard::DashboardChannel& channel = *resolved.channel;
        const dashboard::DashboardConversion& conversion = *resolved.conversion;
        rows_.push_back({.card_id = QString::fromStdString(card.id),
                         .channel_id = QString::fromStdString(card.channel_id),
                         .title = QString::fromStdString(card.title.value_or(channel.name)),
                         .unit = QString::fromStdString(conversion.unit),
                         .precision = conversion.precision});
        rows_by_channel_.emplace(card.channel_id, static_cast<int>(rows_.size() - 1));
    }
}

int DashboardCardModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

QVariant DashboardCardModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
    {
        return {};
    }
    const Row& row = rows_.at(index.row());
    switch (role)
    {
    case CardIdRole:
        return row.card_id;
    case ChannelIdRole:
        return row.channel_id;
    case TitleRole:
        return row.title;
    case FormattedValueRole:
        return row.has_reading ? QString::number(row.numeric_value, 'f', row.precision) : QStringLiteral("—");
    case NumericValueRole:
        return row.has_reading ? QVariant{row.numeric_value} : QVariant{};
    case UnitRole:
        return row.unit;
    case PrecisionRole:
        return row.precision;
    case ReadingStateRole:
        return QVariant::fromValue(row.reading_state);
    case HasReadingRole:
        return row.has_reading;
    case LastUpdateAgeTextRole:
        return ageText(row);
    default:
        return {};
    }
}

QHash<int, QByteArray> DashboardCardModel::roleNames() const
{
    return {
        {CardIdRole, "cardId"},
        {ChannelIdRole, "channelId"},
        {TitleRole, "title"},
        {FormattedValueRole, "formattedValue"},
        {NumericValueRole, "numericValue"},
        {UnitRole, "unit"},
        {PrecisionRole, "precision"},
        {ReadingStateRole, "readingState"},
        {HasReadingRole, "hasReading"},
        {LastUpdateAgeTextRole, "lastUpdateAgeText"},
    };
}

void DashboardCardModel::applySamples(const QVector<logging::LogSample>& samples, std::uint64_t now_ms, bool running)
{
    QVector<ReceivedLogSample> received_samples;
    received_samples.reserve(samples.size());
    for (const logging::LogSample& sample : samples)
    {
        received_samples.push_back({.sample = sample, .received_at_ms = now_ms});
    }
    applySamples(received_samples, running);
}

void DashboardCardModel::applySamples(const QVector<ReceivedLogSample>& samples, bool running)
{
    QVector<QVector<int>> changed_roles(rows_.size());
    for (const ReceivedLogSample& received : samples)
    {
        const logging::LogSample& sample = received.sample;
        const auto it = rows_by_channel_.find(sample.channel_id);
        if (it == rows_by_channel_.end() || !std::isfinite(sample.numeric_value))
        {
            continue;
        }

        Row& row = rows_[it->second];
        QVector<int>& roles = changed_roles[it->second];
        const bool has_reading = row.has_reading;
        const double old_value = row.numeric_value;
        const ReadingState old_state = row.reading_state;
        const QString old_age_text = ageText(row);
        row.numeric_value = sample.numeric_value;
        row.last_update_ms = received.received_at_ms;
        row.age_seconds = 0;
        row.has_reading = true;
        row.reading_state = running ? ReadingState::Live : ReadingState::Stale;
        if (!has_reading || old_value != row.numeric_value)
        {
            roles.append(FormattedValueRole);
            roles.append(NumericValueRole);
        }
        if (old_state != row.reading_state)
        {
            roles.append(ReadingStateRole);
        }
        if (!has_reading)
        {
            roles.append(HasReadingRole);
        }
        if (old_age_text != ageText(row))
        {
            roles.append(LastUpdateAgeTextRole);
        }
    }

    notifyChangedRows(changed_roles);
}

void DashboardCardModel::markReceivedRowsStale()
{
    QVector<QVector<int>> changed_roles(rows_.size());
    for (int row_index = 0; row_index < rowCount(); ++row_index)
    {
        Row& row = rows_[row_index];
        if (!row.has_reading || row.reading_state == ReadingState::Stale)
        {
            continue;
        }
        row.reading_state = ReadingState::Stale;
        changed_roles[row_index] = {ReadingStateRole};
    }
    notifyChangedRows(changed_roles);
}

void DashboardCardModel::updateAges(std::uint64_t now_ms)
{
    QVector<QVector<int>> changed_roles(rows_.size());
    for (int row_index = 0; row_index < rowCount(); ++row_index)
    {
        Row& row = rows_[row_index];
        if (!row.has_reading || now_ms < row.last_update_ms)
        {
            continue;
        }
        const std::uint64_t age_seconds = (now_ms - row.last_update_ms) / 1000;
        if (age_seconds <= row.age_seconds)
        {
            continue;
        }
        row.age_seconds = age_seconds;
        changed_roles[row_index] = {LastUpdateAgeTextRole};
    }
    notifyChangedRows(changed_roles);
}

bool DashboardCardModel::hasReceivedRows() const
{
    return std::any_of(rows_.cbegin(), rows_.cend(), [](const Row& row) { return row.has_reading; });
}

bool DashboardCardModel::containsChannel(std::string_view channel_id) const
{
    return rows_by_channel_.contains(std::string(channel_id));
}

void DashboardCardModel::notifyChangedRows(const QVector<QVector<int>>& roles_by_row)
{
    for (int first_row = 0; first_row < roles_by_row.size();)
    {
        if (roles_by_row[first_row].isEmpty())
        {
            ++first_row;
            continue;
        }

        int last_row = first_row;
        QVector<int> distinct_roles;
        while (last_row < roles_by_row.size() && !roles_by_row[last_row].isEmpty())
        {
            for (const int role : roles_by_row[last_row])
            {
                if (!distinct_roles.contains(role))
                {
                    distinct_roles.append(role);
                }
            }
            ++last_row;
        }
        emit dataChanged(index(first_row, 0), index(last_row - 1, 0), distinct_roles);
        first_row = last_row;
    }
}

QString DashboardCardModel::ageText(const Row& row) const
{
    return row.has_reading ? QStringLiteral("Last update %1s ago").arg(row.age_seconds) : QString{};
}

} // namespace fastecu::desktop_quick
