#include "src/ui/desktop-quick/dashboard/dashboard_editor_model.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <utility>

#include <QVariantMap>

namespace fastecu::desktop_quick
{
namespace
{

QString to_qstring(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string to_string(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

std::optional<dashboard::CardDisplayType> to_domain_display_type(CardDisplayType display_type)
{
    switch (display_type)
    {
    case CardDisplayType::Numeric:
        return dashboard::CardDisplayType::Numeric;
    case CardDisplayType::Sparkline:
        return dashboard::CardDisplayType::Sparkline;
    case CardDisplayType::HorizontalGauge:
        return dashboard::CardDisplayType::HorizontalGauge;
    }
    return std::nullopt;
}

CardDisplayType to_editor_display_type(dashboard::CardDisplayType display_type)
{
    switch (display_type)
    {
    case dashboard::CardDisplayType::Numeric:
        return CardDisplayType::Numeric;
    case dashboard::CardDisplayType::Sparkline:
        return CardDisplayType::Sparkline;
    case dashboard::CardDisplayType::HorizontalGauge:
        return CardDisplayType::HorizontalGauge;
    }
    std::unreachable();
}

} // namespace

DashboardEditorModel::DashboardEditorModel(DashboardDocumentController& controller, QObject *parent)
    : QAbstractListModel(parent), controller_(controller)
{
    connect(&controller_, &DashboardDocumentController::documentCommitted, this,
            &DashboardEditorModel::resetFromCommittedDocument);
    connect(&controller_, &DashboardDocumentController::stateChanged, this, &DashboardEditorModel::refreshProperties);
    if (const dashboard::DashboardDocument *current = document(); current != nullptr && !current->channels.empty())
    {
        add_channel_id_ = to_qstring(current->channels.front().id);
    }
}

int DashboardEditorModel::rowCount(const QModelIndex& parent) const
{
    const dashboard::DashboardDocument *current = document();
    return parent.isValid() || current == nullptr ? 0 : static_cast<int>(current->cards.size());
}

QVariant DashboardEditorModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
    {
        return {};
    }
    const dashboard::DashboardDocument& current = *document();
    const std::vector<std::size_t> indexes = orderedCardIndexes();
    const dashboard::DashboardCard& card = current.cards.at(indexes.at(static_cast<std::size_t>(index.row())));
    const dashboard::DashboardChannel *channel = channelFor(card);

    switch (role)
    {
    case CardIdRole:
        return to_qstring(card.id);
    case TitleRole:
        return to_qstring(card.title.value_or(channel == nullptr ? std::string{} : channel->name));
    case ChannelNameRole:
        return channel == nullptr ? QString{} : to_qstring(channel->name);
    case ConversionIdRole:
        return to_qstring(card.conversion_id);
    case DisplayTypeRole:
        return QVariant::fromValue(to_editor_display_type(card.display_type));
    default:
        return {};
    }
}

QHash<int, QByteArray> DashboardEditorModel::roleNames() const
{
    return {
        {CardIdRole, "cardId"},           {TitleRole, "title"},
        {ChannelNameRole, "channelName"}, {ConversionIdRole, "conversionId"},
        {DisplayTypeRole, "displayType"},
    };
}

QString DashboardEditorModel::selectedCardId() const
{
    return controller_.selectedCardId();
}

QVariantList DashboardEditorModel::channelChoices() const
{
    QVariantList choices;
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return choices;
    }
    choices.reserve(static_cast<qsizetype>(current->channels.size()));
    for (const dashboard::DashboardChannel& channel : current->channels)
    {
        choices.append(QVariantMap{{QStringLiteral("id"), to_qstring(channel.id)},
                                   {QStringLiteral("name"), to_qstring(channel.name)}});
    }
    return choices;
}

QVariantList DashboardEditorModel::conversionChoices() const
{
    const dashboard::DashboardCard *card = selectedCard();
    return card == nullptr ? QVariantList{} : conversionChoicesForChannel(to_qstring(card->channel_id));
}

QString DashboardEditorModel::addChannelId() const
{
    return add_channel_id_;
}

QVariantList DashboardEditorModel::addConversionChoices() const
{
    return conversionChoicesForChannel(add_channel_id_);
}

QVariantList DashboardEditorModel::conversionChoicesForChannel(const QString& channel_id) const
{
    QVariantList choices;
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return choices;
    }
    const std::string requested_channel = to_string(channel_id);
    const auto found = std::ranges::find(current->channels, requested_channel, &dashboard::DashboardChannel::id);
    if (found == current->channels.end())
    {
        return choices;
    }
    choices.reserve(static_cast<qsizetype>(found->conversions.size()));
    for (const dashboard::DashboardConversion& conversion : found->conversions)
    {
        choices.append(QVariantMap{{QStringLiteral("id"), to_qstring(conversion.id)},
                                   {QStringLiteral("unit"), to_qstring(conversion.unit)}});
    }
    return choices;
}

QVariantList DashboardEditorModel::displayTypeChoices() const
{
    return {
        QVariantMap{{QStringLiteral("value"), static_cast<int>(CardDisplayType::Numeric)},
                    {QStringLiteral("label"), QStringLiteral("Numeric")}},
        QVariantMap{{QStringLiteral("value"), static_cast<int>(CardDisplayType::Sparkline)},
                    {QStringLiteral("label"), QStringLiteral("Sparkline")}},
        QVariantMap{{QStringLiteral("value"), static_cast<int>(CardDisplayType::HorizontalGauge)},
                    {QStringLiteral("label"), QStringLiteral("Horizontal Gauge")}},
    };
}

void DashboardEditorModel::setAddChannel(const QString& channel_id)
{
    if (channel_id == add_channel_id_)
    {
        return;
    }
    add_channel_id_ = channel_id;
    emit addChoicesChanged();
}

QString DashboardEditorModel::selectedChannelId() const
{
    const dashboard::DashboardCard *card = selectedCard();
    return card == nullptr ? QString{} : to_qstring(card->channel_id);
}

QString DashboardEditorModel::selectedConversionId() const
{
    const dashboard::DashboardCard *card = selectedCard();
    return card == nullptr ? QString{} : to_qstring(card->conversion_id);
}

QString DashboardEditorModel::selectedTitle() const
{
    const dashboard::DashboardCard *card = selectedCard();
    if (card == nullptr)
    {
        return {};
    }
    const dashboard::DashboardChannel *channel = channelFor(*card);
    return to_qstring(card->title.value_or(channel == nullptr ? std::string{} : channel->name));
}

CardDisplayType DashboardEditorModel::selectedDisplayType() const
{
    const dashboard::DashboardCard *card = selectedCard();
    return card == nullptr ? CardDisplayType::Numeric : to_editor_display_type(card->display_type);
}

double DashboardEditorModel::selectedGaugeMinimum() const
{
    const dashboard::DashboardCard *card = selectedCard();
    const dashboard::DashboardConversion *conversion = card == nullptr ? nullptr : conversionFor(*card);
    return card != nullptr && card->gauge_bounds ? card->gauge_bounds->minimum
                                                 : (conversion == nullptr ? 0.0 : conversion->gauge_min);
}

double DashboardEditorModel::selectedGaugeMaximum() const
{
    const dashboard::DashboardCard *card = selectedCard();
    const dashboard::DashboardConversion *conversion = card == nullptr ? nullptr : conversionFor(*card);
    return card != nullptr && card->gauge_bounds ? card->gauge_bounds->maximum
                                                 : (conversion == nullptr ? 1.0 : conversion->gauge_max);
}

double DashboardEditorModel::selectedGaugeStep() const
{
    const dashboard::DashboardCard *card = selectedCard();
    const dashboard::DashboardConversion *conversion = card == nullptr ? nullptr : conversionFor(*card);
    return card != nullptr && card->gauge_bounds ? card->gauge_bounds->step
                                                 : (conversion == nullptr ? 1.0 : conversion->gauge_step);
}

int DashboardEditorModel::selectedSparklineHistorySeconds() const
{
    const dashboard::DashboardCard *card = selectedCard();
    return card == nullptr ? 0 : static_cast<int>(card->sparkline_history_seconds.value_or(0));
}

bool DashboardEditorModel::canAdd() const
{
    const dashboard::DashboardDocument *current = document();
    if (!controller_.editingEnabled() || current == nullptr)
    {
        return false;
    }
    return std::ranges::any_of(current->channels,
                               [current](const dashboard::DashboardChannel& channel)
                               {
                                   return std::ranges::none_of(current->cards,
                                                               [&channel](const dashboard::DashboardCard& card)
                                                               { return card.channel_id == channel.id; });
                               });
}

bool DashboardEditorModel::canRemove() const
{
    return controller_.editingEnabled() && selectedCard() != nullptr;
}

bool DashboardEditorModel::canMoveUp() const
{
    if (!controller_.editingEnabled())
    {
        return false;
    }
    const std::vector<std::size_t> indexes = orderedCardIndexes();
    const dashboard::DashboardDocument *current = document();
    const std::string selected = to_string(selectedCardId());
    return current != nullptr && !indexes.empty() && current->cards.at(indexes.front()).id != selected &&
           selectedCard() != nullptr;
}

bool DashboardEditorModel::canMoveDown() const
{
    if (!controller_.editingEnabled())
    {
        return false;
    }
    const std::vector<std::size_t> indexes = orderedCardIndexes();
    const dashboard::DashboardDocument *current = document();
    const std::string selected = to_string(selectedCardId());
    return current != nullptr && !indexes.empty() && current->cards.at(indexes.back()).id != selected &&
           selectedCard() != nullptr;
}

void DashboardEditorModel::selectCard(const QString& card_id)
{
    if (const dashboard::DashboardDocument *current = document(); current != nullptr)
    {
        commit(*current, card_id);
    }
}

void DashboardEditorModel::addCard(const QString& channel_id, const QString& conversion_id)
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return;
    }
    dashboard::DashboardDocument candidate = *current;
    orderCards(candidate.cards);
    const std::string channel = to_string(channel_id);
    std::size_t suffix = 1;
    std::string card_id;
    do
    {
        card_id = channel + "-" + std::to_string(suffix++);
    } while (std::ranges::any_of(candidate.cards,
                                 [&card_id](const dashboard::DashboardCard& card) { return card.id == card_id; }));
    candidate.cards.push_back(dashboard::DashboardCard{
        .id = card_id,
        .channel_id = channel,
        .conversion_id = to_string(conversion_id),
        .display_type = dashboard::CardDisplayType::Numeric,
        .title = std::nullopt,
        .order = static_cast<std::uint32_t>(candidate.cards.size()),
        .gauge_bounds = std::nullopt,
        .sparkline_history_seconds = std::nullopt,
    });
    orderCards(candidate.cards);
    commit(std::move(candidate), to_qstring(card_id));
}

void DashboardEditorModel::removeSelected()
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return;
    }
    dashboard::DashboardDocument candidate = *current;
    orderCards(candidate.cards);
    const std::string selected = to_string(selectedCardId());
    const auto found = std::ranges::find(candidate.cards, selected, &dashboard::DashboardCard::id);
    QString next_selection = selectedCardId();
    if (found != candidate.cards.end())
    {
        const std::size_t index = static_cast<std::size_t>(std::distance(candidate.cards.begin(), found));
        candidate.cards.erase(found);
        if (candidate.cards.empty())
        {
            next_selection.clear();
        }
        else
        {
            next_selection = to_qstring(candidate.cards.at(std::min(index, candidate.cards.size() - 1)).id);
        }
    }
    orderCards(candidate.cards);
    commit(std::move(candidate), next_selection);
}

void DashboardEditorModel::moveSelectedUp()
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return;
    }
    dashboard::DashboardDocument candidate = *current;
    orderCards(candidate.cards);
    const auto found = std::ranges::find(candidate.cards, to_string(selectedCardId()), &dashboard::DashboardCard::id);
    if (found != candidate.cards.end() && found != candidate.cards.begin())
    {
        std::iter_swap(found, std::prev(found));
    }
    renumberCards(candidate.cards);
    commit(std::move(candidate), selectedCardId());
}

void DashboardEditorModel::moveSelectedDown()
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return;
    }
    dashboard::DashboardDocument candidate = *current;
    orderCards(candidate.cards);
    const auto found = std::ranges::find(candidate.cards, to_string(selectedCardId()), &dashboard::DashboardCard::id);
    if (found != candidate.cards.end() && std::next(found) != candidate.cards.end())
    {
        std::iter_swap(found, std::next(found));
    }
    renumberCards(candidate.cards);
    commit(std::move(candidate), selectedCardId());
}

void DashboardEditorModel::setSelectedChannel(const QString& channel_id)
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return;
    }
    dashboard::DashboardDocument candidate = *current;
    dashboard::DashboardCard *card = selectedCard(candidate, selectedCardId());
    const std::string channel = to_string(channel_id);
    if (card != nullptr && card->channel_id != channel)
    {
        card->channel_id = channel;
        const auto found = std::ranges::find(candidate.channels, channel, &dashboard::DashboardChannel::id);
        card->conversion_id = found == candidate.channels.end() || found->conversions.empty()
                                  ? std::string{}
                                  : found->conversions.front().id;
    }
    commit(std::move(candidate), selectedCardId());
}

void DashboardEditorModel::setSelectedConversion(const QString& conversion_id)
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return;
    }
    dashboard::DashboardDocument candidate = *current;
    if (dashboard::DashboardCard *card = selectedCard(candidate, selectedCardId()); card != nullptr)
    {
        card->conversion_id = to_string(conversion_id);
    }
    commit(std::move(candidate), selectedCardId());
}

void DashboardEditorModel::setSelectedTitle(const QString& title)
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return;
    }
    dashboard::DashboardDocument candidate = *current;
    if (dashboard::DashboardCard *card = selectedCard(candidate, selectedCardId()); card != nullptr)
    {
        const std::string next_title = to_string(title);
        const auto channel = std::ranges::find(candidate.channels, card->channel_id, &dashboard::DashboardChannel::id);
        const bool projected_fallback =
            !card->title.has_value() && channel != candidate.channels.end() && next_title == channel->name;
        if (!projected_fallback)
        {
            card->title = title.isEmpty() ? std::nullopt : std::optional<std::string>{next_title};
        }
    }
    commit(std::move(candidate), selectedCardId());
}

void DashboardEditorModel::setSelectedDisplayType(CardDisplayType display_type)
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return;
    }
    dashboard::DashboardDocument candidate = *current;
    dashboard::DashboardCard *card = selectedCard(candidate, selectedCardId());
    if (card == nullptr)
    {
        commit(std::move(candidate), selectedCardId());
        return;
    }
    const std::optional<dashboard::CardDisplayType> next = to_domain_display_type(display_type);
    if (!next.has_value())
    {
        card->display_type = static_cast<dashboard::CardDisplayType>(-1);
        commit(std::move(candidate), selectedCardId());
        return;
    }
    if (card->display_type == *next)
    {
        commit(std::move(candidate), selectedCardId());
        return;
    }
    card->display_type = *next;
    switch (*next)
    {
    case dashboard::CardDisplayType::Numeric:
        card->gauge_bounds.reset();
        card->sparkline_history_seconds.reset();
        break;
    case dashboard::CardDisplayType::Sparkline:
        card->gauge_bounds.reset();
        card->sparkline_history_seconds = 60;
        break;
    case dashboard::CardDisplayType::HorizontalGauge:
    {
        card->sparkline_history_seconds.reset();
        const dashboard::DashboardChannel *channel = nullptr;
        const auto channel_it =
            std::ranges::find(candidate.channels, card->channel_id, &dashboard::DashboardChannel::id);
        if (channel_it != candidate.channels.end())
        {
            channel = &*channel_it;
        }
        const dashboard::DashboardConversion *conversion = nullptr;
        if (channel != nullptr)
        {
            const auto conversion_it =
                std::ranges::find(channel->conversions, card->conversion_id, &dashboard::DashboardConversion::id);
            if (conversion_it != channel->conversions.end())
            {
                conversion = &*conversion_it;
            }
        }
        card->gauge_bounds =
            conversion == nullptr
                ? dashboard::GaugeBoundsOverride{0.0, 1.0, 1.0}
                : dashboard::GaugeBoundsOverride{conversion->gauge_min, conversion->gauge_max, conversion->gauge_step};
        break;
    }
    }
    commit(std::move(candidate), selectedCardId());
}

void DashboardEditorModel::setSelectedGaugeBounds(double minimum, double maximum, double step)
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return;
    }
    dashboard::DashboardDocument candidate = *current;
    if (dashboard::DashboardCard *card = selectedCard(candidate, selectedCardId()); card != nullptr)
    {
        card->gauge_bounds = dashboard::GaugeBoundsOverride{minimum, maximum, step};
    }
    commit(std::move(candidate), selectedCardId());
}

void DashboardEditorModel::setSelectedSparklineHistorySeconds(int seconds)
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return;
    }
    dashboard::DashboardDocument candidate = *current;
    if (dashboard::DashboardCard *card = selectedCard(candidate, selectedCardId()); card != nullptr)
    {
        const int bounded = seconds < 0 || seconds > std::numeric_limits<std::uint16_t>::max() ? 0 : seconds;
        card->sparkline_history_seconds = static_cast<std::uint16_t>(bounded);
    }
    commit(std::move(candidate), selectedCardId());
}

const dashboard::DashboardDocument *DashboardEditorModel::document() const
{
    const auto& current = controller_.document();
    return current ? &*current : nullptr;
}

const dashboard::DashboardCard *DashboardEditorModel::selectedCard() const
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return nullptr;
    }
    const auto found = std::ranges::find(current->cards, to_string(selectedCardId()), &dashboard::DashboardCard::id);
    return found == current->cards.end() ? nullptr : &*found;
}

const dashboard::DashboardChannel *DashboardEditorModel::channelFor(const dashboard::DashboardCard& card) const
{
    const dashboard::DashboardDocument *current = document();
    if (current == nullptr)
    {
        return nullptr;
    }
    const auto found = std::ranges::find(current->channels, card.channel_id, &dashboard::DashboardChannel::id);
    return found == current->channels.end() ? nullptr : &*found;
}

const dashboard::DashboardConversion *DashboardEditorModel::conversionFor(const dashboard::DashboardCard& card) const
{
    const dashboard::DashboardChannel *channel = channelFor(card);
    if (channel == nullptr)
    {
        return nullptr;
    }
    const auto found = std::ranges::find(channel->conversions, card.conversion_id, &dashboard::DashboardConversion::id);
    return found == channel->conversions.end() ? nullptr : &*found;
}

std::vector<std::size_t> DashboardEditorModel::orderedCardIndexes() const
{
    const dashboard::DashboardDocument *current = document();
    std::vector<std::size_t> indexes;
    if (current == nullptr)
    {
        return indexes;
    }
    indexes.reserve(current->cards.size());
    for (std::size_t index = 0; index < current->cards.size(); ++index)
    {
        indexes.push_back(index);
    }
    std::ranges::stable_sort(indexes, {}, [current](std::size_t index) { return current->cards.at(index).order; });
    return indexes;
}

void DashboardEditorModel::orderCards(std::vector<dashboard::DashboardCard>& cards)
{
    std::ranges::stable_sort(cards, {}, &dashboard::DashboardCard::order);
    renumberCards(cards);
}

void DashboardEditorModel::renumberCards(std::vector<dashboard::DashboardCard>& cards)
{
    for (std::size_t index = 0; index < cards.size(); ++index)
    {
        cards[index].order = static_cast<std::uint32_t>(index);
    }
}

dashboard::DashboardCard *DashboardEditorModel::selectedCard(dashboard::DashboardDocument& document,
                                                             const QString& selected_card_id)
{
    const auto found = std::ranges::find(document.cards, to_string(selected_card_id), &dashboard::DashboardCard::id);
    return found == document.cards.end() ? nullptr : &*found;
}

void DashboardEditorModel::commit(dashboard::DashboardDocument candidate, const QString& selected_card_id)
{
    static_cast<void>(controller_.commitCandidate(std::move(candidate), to_string(selected_card_id)));
}

void DashboardEditorModel::resetFromCommittedDocument()
{
    beginResetModel();
    endResetModel();
    const dashboard::DashboardDocument *current = document();
    const std::string selected = to_string(add_channel_id_);
    const bool selection_exists =
        current != nullptr &&
        std::ranges::any_of(current->channels,
                            [&selected](const dashboard::DashboardChannel& item) { return item.id == selected; });
    if (!selection_exists)
    {
        add_channel_id_ =
            current == nullptr || current->channels.empty() ? QString{} : to_qstring(current->channels.front().id);
    }
    emit addChoicesChanged();
}

void DashboardEditorModel::refreshProperties()
{
    emit selectionChanged();
    emit choicesChanged();
    emit availabilityChanged();
}

} // namespace fastecu::desktop_quick
