#include "src/backend/dashboard/dashboard_validation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "src/backend/logging/logging_session.h"

namespace fastecu::dashboard
{
namespace
{
Status invalid(std::string path, std::string explanation)
{
    return fail(ErrorKind::InvalidConfig, std::move(path) + ": " + std::move(explanation));
}

Status validate_gauge(double minimum, double maximum, double step, std::string path)
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(step))
    {
        return invalid(std::move(path), "values must be finite");
    }
    if (minimum >= maximum)
    {
        return invalid(std::move(path), "minimum must be less than maximum");
    }
    if (step <= 0.0)
    {
        return invalid(std::move(path), "step must be positive");
    }
    return {};
}

std::string item_path(std::string_view collection, std::string_view id, std::size_t index)
{
    return std::string(collection) + "[" + (id.empty() ? std::to_string(index) : std::string(id)) + "]";
}
} // namespace

Status validate_dashboard_document(const DashboardDocument& document)
{
    if (document.metadata.format_version != 1)
    {
        return invalid("metadata.format-version", "only format version 1 is supported");
    }
    if (document.metadata.name.empty())
    {
        return invalid("metadata.name", "must not be empty");
    }

    if (document.connection.protocol != DashboardProtocol::Cdbg)
    {
        return invalid("connection.protocol", "must be CDBG");
    }
    if (document.connection.transport != DashboardTransport::RawCan)
    {
        return invalid("connection.transport", "must be raw CAN");
    }
    if (document.connection.bitrate == 0)
    {
        return invalid("connection.bitrate", "must be positive");
    }

    std::uint32_t maximum_identifier = 0;
    switch (document.connection.identifier_width)
    {
    case CanIdentifierWidth::Standard:
        maximum_identifier = 0x7ff;
        break;
    case CanIdentifierWidth::Extended:
        maximum_identifier = 0x1fffffff;
        break;
    default:
        return invalid("connection.identifier-width", "must be 11 or 29 bits");
    }
    if (document.connection.request_id > maximum_identifier)
    {
        return invalid("connection.request-id", "exceeds the selected identifier width");
    }
    if (document.connection.reply_id > maximum_identifier)
    {
        return invalid("connection.reply-id", "exceeds the selected identifier width");
    }
    if (document.connection.reply_id == document.connection.request_id)
    {
        return invalid("connection.reply-id", "must differ from the request identifier");
    }
    if (document.connection.sampling_interval_ms == 0)
    {
        return invalid("connection.sampling-interval-ms", "must be positive");
    }
    if (document.connection.retry.poll_timeout_ms == 0)
    {
        return invalid("connection.retry.poll-timeout-ms", "must be positive");
    }
    if (document.connection.retry.silence_threshold == 0)
    {
        return invalid("connection.retry.silence-threshold", "must be positive");
    }
    if (document.connection.retry.reconnect_attempts == 0)
    {
        return invalid("connection.retry.reconnect-attempts", "must be positive");
    }
    if (document.connection.retry.reconnect_period_ms == 0)
    {
        return invalid("connection.retry.reconnect-period-ms", "must be positive");
    }
    if (const auto& adapter = document.connection.preferred_adapter; adapter.has_value())
    {
        switch (adapter->kind)
        {
        case AdapterKind::J2534:
        case AdapterKind::SocketCan:
            break;
        default:
            return invalid("connection.preferred-adapter.kind", "is not supported");
        }
        if (adapter->vendor.empty())
        {
            return invalid("connection.preferred-adapter.vendor", "must not be empty");
        }
        if (adapter->display_name.empty())
        {
            return invalid("connection.preferred-adapter.display-name", "must not be empty");
        }
    }

    std::unordered_set<std::string_view> channel_ids;
    for (std::size_t channel_index = 0; channel_index < document.channels.size(); ++channel_index)
    {
        const DashboardChannel& channel = document.channels[channel_index];
        const std::string channel_path = item_path("channels", channel.id, channel_index);
        if (channel.id.empty())
        {
            return invalid(channel_path + ".id", "must not be empty");
        }
        if (!channel_ids.insert(channel.id).second)
        {
            return invalid(channel_path + ".id", "must be unique");
        }
        if (channel.name.empty())
        {
            return invalid(channel_path + ".name", "must not be empty");
        }
        if (channel.description.empty())
        {
            return invalid(channel_path + ".description", "must not be empty");
        }
        if (channel.length != 1 && channel.length != 2 && channel.length != 4)
        {
            return invalid(channel_path + ".length", "must be 1, 2, or 4 bytes");
        }
        if (channel.raw_assembly != RawAssembly::UnsignedIntegerDecimal)
        {
            return invalid(channel_path + ".raw-assembly", "must be unsigned-integer-decimal");
        }
        if (channel.conversions.empty())
        {
            return invalid(channel_path + ".conversions", "must contain at least one conversion");
        }

        std::unordered_set<std::string_view> conversion_ids;
        for (std::size_t conversion_index = 0; conversion_index < channel.conversions.size(); ++conversion_index)
        {
            const DashboardConversion& conversion = channel.conversions[conversion_index];
            const std::string conversion_path =
                channel_path + "." + item_path("conversions", conversion.id, conversion_index);
            if (conversion.id.empty())
            {
                return invalid(conversion_path + ".id", "must not be empty");
            }
            if (!conversion_ids.insert(conversion.id).second)
            {
                return invalid(conversion_path + ".id", "must be unique within its channel");
            }
            if (!logging::valid_conversion_expression(conversion.expression))
            {
                return invalid(conversion_path + ".expression", "is not a valid conversion expression");
            }
            if (conversion.unit.empty())
            {
                return invalid(conversion_path + ".unit", "must not be empty");
            }
            if (!logging::valid_display_precision(conversion.precision))
            {
                return invalid(conversion_path + ".precision", "is not a supported display precision");
            }
            if (Status gauge = validate_gauge(conversion.gauge_min, conversion.gauge_max, conversion.gauge_step,
                                              conversion_path + ".gauge");
                !gauge)
            {
                return gauge;
            }
        }
    }

    std::unordered_set<std::string_view> card_ids;
    std::unordered_set<std::uint32_t> card_orders;
    std::unordered_set<std::string_view> card_channel_ids;
    for (std::size_t card_index = 0; card_index < document.cards.size(); ++card_index)
    {
        const DashboardCard& card = document.cards[card_index];
        const std::string card_path = item_path("cards", card.id, card_index);
        if (card.id.empty())
        {
            return invalid(card_path + ".id", "must not be empty");
        }
        if (!card_ids.insert(card.id).second)
        {
            return invalid(card_path + ".id", "must be unique");
        }

        const auto channel = std::ranges::find(document.channels, card.channel_id, &DashboardChannel::id);
        if (channel == document.channels.end())
        {
            return invalid(card_path + ".channel-id", "does not reference a channel");
        }
        if (!card_channel_ids.insert(card.channel_id).second)
        {
            return invalid(card_path + ".channel-id", "only one card may reference a channel");
        }
        if (std::ranges::find(channel->conversions, card.conversion_id, &DashboardConversion::id) ==
            channel->conversions.end())
        {
            return invalid(card_path + ".conversion-id", "does not reference a conversion on the channel");
        }

        switch (card.display_type)
        {
        case CardDisplayType::Numeric:
        case CardDisplayType::Sparkline:
        case CardDisplayType::HorizontalGauge:
            break;
        default:
            return invalid(card_path + ".display-type", "is not supported");
        }
        if (card.order >= document.cards.size() || !card_orders.insert(card.order).second)
        {
            return invalid(card_path + ".order", "orders must be the unique contiguous range starting at zero");
        }

        switch (card.display_type)
        {
        case CardDisplayType::Numeric:
            if (card.gauge_bounds.has_value())
            {
                return invalid(card_path + ".gauge", "is only valid for a horizontal gauge");
            }
            if (card.sparkline_history_seconds.has_value())
            {
                return invalid(card_path + ".sparkline-history-seconds", "is only valid for a sparkline");
            }
            break;
        case CardDisplayType::Sparkline:
            if (card.gauge_bounds.has_value())
            {
                return invalid(card_path + ".gauge", "is only valid for a horizontal gauge");
            }
            if (!card.sparkline_history_seconds.has_value() || *card.sparkline_history_seconds < 1 ||
                *card.sparkline_history_seconds > 300)
            {
                return invalid(card_path + ".sparkline-history-seconds", "must be from 1 through 300 seconds");
            }
            break;
        case CardDisplayType::HorizontalGauge:
            if (!card.gauge_bounds.has_value())
            {
                return invalid(card_path + ".gauge", "is required for a horizontal gauge");
            }
            if (Status gauge = validate_gauge(card.gauge_bounds->minimum, card.gauge_bounds->maximum,
                                              card.gauge_bounds->step, card_path + ".gauge");
                !gauge)
            {
                return gauge;
            }
            if (card.sparkline_history_seconds.has_value())
            {
                return invalid(card_path + ".sparkline-history-seconds", "is only valid for a sparkline");
            }
            break;
        default:
            break;
        }
    }

    return {};
}
} // namespace fastecu::dashboard
