#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fastecu::dashboard
{
enum class DashboardProtocol
{
    Cdbg
};
enum class DashboardTransport
{
    RawCan
};
enum class CanIdentifierWidth : std::uint8_t
{
    Standard = 11,
    Extended = 29
};
enum class RawAssembly
{
    UnsignedIntegerDecimal
};
enum class AdapterKind
{
    J2534,
    SocketCan
};
enum class CardDisplayType
{
    Numeric,
    Sparkline,
    HorizontalGauge
};

struct DocumentMetadata
{
    std::uint32_t format_version{1};
    std::string name;
    std::optional<std::string> description;
    bool operator==(const DocumentMetadata&) const = default;
};
struct RetryPolicy
{
    std::uint32_t poll_timeout_ms;
    std::uint32_t silence_threshold;
    std::uint32_t reconnect_attempts;
    std::uint32_t reconnect_period_ms;
    bool operator==(const RetryPolicy&) const = default;
};
struct PreferredAdapter
{
    AdapterKind kind;
    std::string vendor;
    std::string display_name;
    bool operator==(const PreferredAdapter&) const = default;
};
struct CdbgConnectionProfile
{
    DashboardProtocol protocol{DashboardProtocol::Cdbg};
    DashboardTransport transport{DashboardTransport::RawCan};
    std::uint32_t bitrate;
    CanIdentifierWidth identifier_width{CanIdentifierWidth::Standard};
    std::uint32_t request_id;
    std::uint32_t reply_id;
    std::uint8_t stream_instance;
    std::uint32_t sampling_interval_ms;
    RetryPolicy retry;
    std::optional<PreferredAdapter> preferred_adapter;
    bool operator==(const CdbgConnectionProfile&) const = default;
};
struct DashboardConversion
{
    std::string id;
    std::string expression;
    std::string unit;
    std::uint8_t precision;
    double gauge_min;
    double gauge_max;
    double gauge_step;
    bool operator==(const DashboardConversion&) const = default;
};
struct DashboardChannel
{
    std::string id;
    std::string name;
    std::string description;
    std::uint32_t address;
    std::uint8_t length;
    RawAssembly raw_assembly{RawAssembly::UnsignedIntegerDecimal};
    std::vector<DashboardConversion> conversions;
    bool operator==(const DashboardChannel&) const = default;
};
struct GaugeBoundsOverride
{
    double minimum;
    double maximum;
    double step;
    bool operator==(const GaugeBoundsOverride&) const = default;
};
struct DashboardCard
{
    std::string id;
    std::string channel_id;
    std::string conversion_id;
    CardDisplayType display_type;
    std::optional<std::string> title;
    std::uint32_t order;
    std::optional<GaugeBoundsOverride> gauge_bounds;
    std::optional<std::uint16_t> sparkline_history_seconds;
    bool operator==(const DashboardCard&) const = default;
};
struct DashboardDocument
{
    DocumentMetadata metadata;
    CdbgConnectionProfile connection;
    std::vector<DashboardChannel> channels;
    std::vector<DashboardCard> cards;
    bool operator==(const DashboardDocument&) const = default;
};
} // namespace fastecu::dashboard
