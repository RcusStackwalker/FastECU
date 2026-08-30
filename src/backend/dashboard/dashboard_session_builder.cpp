#include "src/backend/dashboard/dashboard_session_builder.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/backend/dashboard/dashboard_validation.h"
#include "src/backend/logging/logging_types.h"

namespace fastecu::dashboard
{
namespace
{
std::unexpected<Error> with_context(std::string_view path, const Error& error)
{
    return fail(error.kind, std::string(path) + ": " + error.detail);
}

Result<int> checked_policy_int(std::uint32_t value, std::string_view path)
{
    if (static_cast<std::uintmax_t>(value) > static_cast<std::uintmax_t>(std::numeric_limits<int>::max()))
    {
        return fail(ErrorKind::InvalidConfig, std::string(path) + ": exceeds the generic logging policy integer range");
    }
    return static_cast<int>(value);
}
} // namespace

PreparedDashboardSession::PreparedDashboardSession(logging::LoggingSession session, cdbg::CdbgProtocolConfig config)
    : session_(std::move(session)), config_(std::move(config))
{
}

const logging::LoggingSession& PreparedDashboardSession::session() const
{
    return session_;
}

const cdbg::CdbgProtocolConfig& PreparedDashboardSession::config() const
{
    return config_;
}

std::pair<logging::LoggingSession, cdbg::CdbgProtocolConfig> PreparedDashboardSession::into_parts() &&
{
    return {std::move(session_), std::move(config_)};
}

fastecu::Result<PreparedDashboardSession> prepare_dashboard_session(const DashboardDocument& document)
{
    if (Status status = validate_dashboard_document(document); !status)
    {
        return std::unexpected(status.error());
    }

    std::unordered_map<std::string_view, const DashboardChannel *> channels_by_id;
    std::unordered_map<std::string_view, std::unordered_map<std::string_view, const DashboardConversion *>>
        conversions_by_channel;
    for (const DashboardChannel& channel : document.channels)
    {
        channels_by_id.emplace(channel.id, &channel);
        auto& conversions = conversions_by_channel[channel.id];
        for (const DashboardConversion& conversion : channel.conversions)
        {
            conversions.emplace(conversion.id, &conversion);
        }
    }

    std::unordered_set<std::string_view> referenced_channel_ids;
    std::unordered_map<std::string_view, const DashboardCard *> cards_by_channel;
    for (const DashboardCard& card : document.cards)
    {
        const auto channel = channels_by_id.find(card.channel_id);
        if (channel == channels_by_id.end())
        {
            return fail(ErrorKind::InvalidConfig, "cards[" + card.id + "].channel-id: does not reference a channel");
        }
        const auto conversions = conversions_by_channel.find(card.channel_id);
        if (conversions == conversions_by_channel.end() || !conversions->second.contains(card.conversion_id))
        {
            return fail(ErrorKind::InvalidConfig,
                        "cards[" + card.id + "].conversion-id: does not reference a conversion on the channel");
        }
        referenced_channel_ids.insert(card.channel_id);
        cards_by_channel.emplace(card.channel_id, &card);
    }

    std::vector<logging::LoggingChannel> selected_channels;
    selected_channels.reserve(referenced_channel_ids.size());
    for (const DashboardChannel& channel : document.channels)
    {
        if (!referenced_channel_ids.contains(channel.id))
        {
            continue;
        }
        const DashboardCard& card = *cards_by_channel.at(channel.id);
        const DashboardConversion& conversion = *conversions_by_channel.at(channel.id).at(card.conversion_id);
        selected_channels.push_back(logging::LoggingChannel{
            .id = channel.id,
            .address = channel.address,
            .length = channel.length,
            .raw_assembly = logging::RawAssembly::UnsignedIntegerDecimal,
            .from_byte_expression = conversion.expression,
            .unit = conversion.unit,
            .decimal_precision = conversion.precision,
        });
    }

    auto poll_timeout =
        checked_policy_int(document.connection.retry.poll_timeout_ms, "connection.retry.poll-timeout-ms");
    if (!poll_timeout)
    {
        return std::unexpected(poll_timeout.error());
    }
    auto silence_threshold =
        checked_policy_int(document.connection.retry.silence_threshold, "connection.retry.silence-threshold");
    if (!silence_threshold)
    {
        return std::unexpected(silence_threshold.error());
    }
    auto reconnect_period =
        checked_policy_int(document.connection.retry.reconnect_period_ms, "connection.retry.reconnect-period-ms");
    if (!reconnect_period)
    {
        return std::unexpected(reconnect_period.error());
    }

    logging::LoggingPolicy policy{
        .poll_timeout_ms = *poll_timeout,
        .car_silence_miss_threshold = *silence_threshold,
        .reconnect_initial_delay_ms = *reconnect_period,
        .reconnect_period_ms = *reconnect_period,
        .max_reconnect_attempts = document.connection.retry.reconnect_attempts,
    };
    auto session =
        logging::make_logging_session(logging::LoggingProtocolId::Cdbg, std::move(selected_channels), policy);
    if (!session)
    {
        return with_context("cards", session.error());
    }

    auto config =
        cdbg::make_cdbg_protocol_config(document.connection.request_id, document.connection.reply_id,
                                        document.connection.stream_instance, document.connection.sampling_interval_ms);
    if (!config)
    {
        return with_context("connection", config.error());
    }

    return PreparedDashboardSession(std::move(*session), std::move(*config));
}
} // namespace fastecu::dashboard
