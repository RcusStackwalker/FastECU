#include "src/backend/dashboard/dashboard_codec.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "src/backend/dashboard/dashboard_validation.h"
#include "src/backend/dashboard/dashboard_xml_validation.h"

namespace fastecu::dashboard
{
namespace
{
using AllowedNames = std::initializer_list<std::string_view>;

std::unexpected<Error> invalid(std::string path, std::string explanation)
{
    return fail(ErrorKind::InvalidConfig, std::move(path) + ": " + std::move(explanation));
}

bool contains(AllowedNames allowed, std::string_view name)
{
    return std::ranges::find(allowed, name) != allowed.end();
}

Status reject_unknown_attributes(pugi::xml_node node, AllowedNames allowed, std::string_view path)
{
    for (const pugi::xml_attribute attribute : node.attributes())
    {
        if (!contains(allowed, attribute.name()))
        {
            return invalid(std::string(path), std::format("unknown attribute '{}'", attribute.name()));
        }
    }
    return {};
}

Status reject_unknown_children(pugi::xml_node node, AllowedNames allowed, std::string_view path)
{
    for (const pugi::xml_node child : node.children())
    {
        if (child.type() != pugi::node_element)
        {
            return invalid(std::string(path), "unexpected text content");
        }
        if (!contains(allowed, child.name()))
        {
            return invalid(std::string(path), std::format("unknown element <{}>", child.name()));
        }
    }
    return {};
}

Result<pugi::xml_node> require_single_child(pugi::xml_node parent, const char *name, std::string_view path)
{
    pugi::xml_node found;
    std::size_t count = 0;
    for (const pugi::xml_node child : parent.children(name))
    {
        found = child;
        ++count;
    }
    if (count == 0)
    {
        return invalid(std::string(path), std::format("missing required <{}>", name));
    }
    if (count != 1)
    {
        return invalid(std::string(path), std::format("expected exactly one <{}>", name));
    }
    return found;
}

Result<std::optional<pugi::xml_node>> optional_single_child(pugi::xml_node parent, const char *name,
                                                            std::string_view path)
{
    pugi::xml_node found;
    std::size_t count = 0;
    for (const pugi::xml_node child : parent.children(name))
    {
        found = child;
        ++count;
    }
    if (count > 1)
    {
        return invalid(std::string(path), std::format("expected at most one <{}>", name));
    }
    return count == 1 ? std::optional<pugi::xml_node>{found} : std::nullopt;
}

Result<std::string> parse_required_string(pugi::xml_node node, const char *name, std::string path)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    if (!attribute)
    {
        return invalid(std::move(path), std::format("missing required attribute '{}'", name));
    }
    return std::string(attribute.value());
}

std::optional<std::string> parse_optional_string(pugi::xml_node node, const char *name)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    return attribute ? std::optional<std::string>{attribute.value()} : std::nullopt;
}

template <typename Integer>
Result<Integer> parse_required_unsigned(pugi::xml_node node, const char *name, std::string path, int base = 10)
{
    auto text = parse_required_string(node, name, path);
    if (!text)
    {
        return std::unexpected(text.error());
    }

    std::string_view digits = *text;
    if (base == 16)
    {
        if (!digits.starts_with("0x") || digits.size() == 2)
        {
            return invalid(std::move(path), "expected hexadecimal text beginning with '0x'");
        }
        digits.remove_prefix(2);
    }

    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (error != std::errc{} || end != digits.data() + digits.size() ||
        value > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max()))
    {
        return invalid(std::move(path), base == 16 ? "invalid or out-of-range hexadecimal integer"
                                                   : "invalid or out-of-range decimal integer");
    }
    return static_cast<Integer>(value);
}

Result<std::uint32_t> parse_required_u32(pugi::xml_node node, const char *name, std::string path, int base = 10)
{
    return parse_required_unsigned<std::uint32_t>(node, name, std::move(path), base);
}

Result<std::string> parse_format_version(pugi::xml_node root)
{
    constexpr std::string_view path = "metadata.format-version";
    auto text = parse_required_string(root, "format-version", std::string(path));
    if (!text)
    {
        return std::unexpected(text.error());
    }
    if (text->empty() || !std::ranges::all_of(*text, [](char digit) { return digit >= '0' && digit <= '9'; }))
    {
        return invalid(std::string(path), "expected an unsigned decimal integer");
    }
    return text;
}

bool is_format_version_one(std::string_view digits)
{
    while (digits.size() > 1 && digits.front() == '0')
    {
        digits.remove_prefix(1);
    }
    return digits == "1";
}

Result<double> parse_required_double(pugi::xml_node node, const char *name, std::string path)
{
    auto text = parse_required_string(node, name, path);
    if (!text)
    {
        return std::unexpected(text.error());
    }
    double value = 0.0;
    const auto [end, error] =
        std::from_chars(text->data(), text->data() + text->size(), value, std::chars_format::general);
    if (error != std::errc{} || end != text->data() + text->size() || !std::isfinite(value))
    {
        return invalid(std::move(path), "expected a finite floating-point number");
    }
    return value;
}

template <typename Enum>
Result<Enum> parse_required_enum(pugi::xml_node node, const char *name, std::string path,
                                 std::initializer_list<std::pair<std::string_view, Enum>> values)
{
    auto text = parse_required_string(node, name, path);
    if (!text)
    {
        return std::unexpected(text.error());
    }
    for (const auto& [spelling, value] : values)
    {
        if (*text == spelling)
        {
            return value;
        }
    }
    return invalid(std::move(path), std::format("unsupported value '{}'", *text));
}

std::string item_path(std::string_view collection, pugi::xml_node node, std::size_t index)
{
    const pugi::xml_attribute id = node.attribute("id");
    return std::string(collection) + "[" + (id ? std::string(id.value()) : std::to_string(index)) + "]";
}

Result<RetryPolicy> decode_retry(pugi::xml_node node)
{
    constexpr std::string_view path = "connection.retry";
    if (Status status = reject_unknown_attributes(
            node, {"poll-timeout-ms", "silence-threshold", "reconnect-attempts", "reconnect-period-ms"}, path);
        !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(node, {}, path); !status)
    {
        return std::unexpected(status.error());
    }

    auto poll_timeout = parse_required_u32(node, "poll-timeout-ms", "connection.retry.poll-timeout-ms");
    auto silence = parse_required_u32(node, "silence-threshold", "connection.retry.silence-threshold");
    auto attempts = parse_required_u32(node, "reconnect-attempts", "connection.retry.reconnect-attempts");
    auto period = parse_required_u32(node, "reconnect-period-ms", "connection.retry.reconnect-period-ms");
    if (!poll_timeout)
    {
        return std::unexpected(poll_timeout.error());
    }
    if (!silence)
    {
        return std::unexpected(silence.error());
    }
    if (!attempts)
    {
        return std::unexpected(attempts.error());
    }
    if (!period)
    {
        return std::unexpected(period.error());
    }
    return RetryPolicy{*poll_timeout, *silence, *attempts, *period};
}

Result<PreferredAdapter> decode_adapter(pugi::xml_node node)
{
    constexpr std::string_view path = "connection.preferred-adapter";
    if (Status status = reject_unknown_attributes(node, {"kind", "vendor", "display-name"}, path); !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(node, {}, path); !status)
    {
        return std::unexpected(status.error());
    }
    auto kind =
        parse_required_enum<AdapterKind>(node, "kind", "connection.preferred-adapter.kind",
                                         {{"j2534", AdapterKind::J2534}, {"socketcan", AdapterKind::SocketCan}});
    auto vendor = parse_required_string(node, "vendor", "connection.preferred-adapter.vendor");
    auto display_name = parse_required_string(node, "display-name", "connection.preferred-adapter.display-name");
    if (!kind)
    {
        return std::unexpected(kind.error());
    }
    if (!vendor)
    {
        return std::unexpected(vendor.error());
    }
    if (!display_name)
    {
        return std::unexpected(display_name.error());
    }
    return PreferredAdapter{*kind, std::move(*vendor), std::move(*display_name)};
}

Result<DashboardConversion> decode_conversion(pugi::xml_node node, const std::string& path)
{
    if (Status status = reject_unknown_attributes(
            node, {"id", "expression", "unit", "precision", "gauge-min", "gauge-max", "gauge-step"}, path);
        !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(node, {}, path); !status)
    {
        return std::unexpected(status.error());
    }

    auto id = parse_required_string(node, "id", path + ".id");
    auto expression = parse_required_string(node, "expression", path + ".expression");
    auto unit = parse_required_string(node, "unit", path + ".unit");
    auto precision = parse_required_unsigned<std::uint8_t>(node, "precision", path + ".precision");
    auto gauge_min = parse_required_double(node, "gauge-min", path + ".gauge-min");
    auto gauge_max = parse_required_double(node, "gauge-max", path + ".gauge-max");
    auto gauge_step = parse_required_double(node, "gauge-step", path + ".gauge-step");
    if (!id)
    {
        return std::unexpected(id.error());
    }
    if (!expression)
    {
        return std::unexpected(expression.error());
    }
    if (!unit)
    {
        return std::unexpected(unit.error());
    }
    if (!precision)
    {
        return std::unexpected(precision.error());
    }
    if (!gauge_min)
    {
        return std::unexpected(gauge_min.error());
    }
    if (!gauge_max)
    {
        return std::unexpected(gauge_max.error());
    }
    if (!gauge_step)
    {
        return std::unexpected(gauge_step.error());
    }
    return DashboardConversion{
        std::move(*id), std::move(*expression), std::move(*unit), *precision, *gauge_min, *gauge_max, *gauge_step};
}

Result<DashboardChannel> decode_channel(pugi::xml_node node, std::size_t index)
{
    const std::string path = item_path("channels", node, index);
    if (Status status =
            reject_unknown_attributes(node, {"id", "name", "description", "address", "length", "raw-assembly"}, path);
        !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(node, {"conversion"}, path); !status)
    {
        return std::unexpected(status.error());
    }

    auto id = parse_required_string(node, "id", path + ".id");
    auto name = parse_required_string(node, "name", path + ".name");
    auto description = parse_required_string(node, "description", path + ".description");
    auto address = parse_required_u32(node, "address", path + ".address", 16);
    auto length = parse_required_unsigned<std::uint8_t>(node, "length", path + ".length");
    auto raw_assembly =
        parse_required_enum<RawAssembly>(node, "raw-assembly", path + ".raw-assembly",
                                         {{"unsigned-integer-decimal", RawAssembly::UnsignedIntegerDecimal}});
    if (!id)
    {
        return std::unexpected(id.error());
    }
    if (!name)
    {
        return std::unexpected(name.error());
    }
    if (!description)
    {
        return std::unexpected(description.error());
    }
    if (!address)
    {
        return std::unexpected(address.error());
    }
    if (!length)
    {
        return std::unexpected(length.error());
    }
    if (!raw_assembly)
    {
        return std::unexpected(raw_assembly.error());
    }

    std::vector<DashboardConversion> conversions;
    std::size_t conversion_index = 0;
    for (const pugi::xml_node conversion_node : node.children("conversion"))
    {
        const std::string conversion_path = path + "." + item_path("conversions", conversion_node, conversion_index);
        auto conversion = decode_conversion(conversion_node, conversion_path);
        if (!conversion)
        {
            return std::unexpected(conversion.error());
        }
        conversions.push_back(std::move(*conversion));
        ++conversion_index;
    }

    return DashboardChannel{std::move(*id), std::move(*name), std::move(*description), *address,
                            *length,        *raw_assembly,    std::move(conversions)};
}

Result<DashboardCard> decode_card(pugi::xml_node node, std::size_t index)
{
    const std::string path = item_path("cards", node, index);
    if (Status status =
            reject_unknown_attributes(node,
                                      {"id", "channel-id", "conversion-id", "display-type", "title", "order",
                                       "gauge-min", "gauge-max", "gauge-step", "sparkline-history-seconds"},
                                      path);
        !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(node, {}, path); !status)
    {
        return std::unexpected(status.error());
    }

    auto id = parse_required_string(node, "id", path + ".id");
    auto channel_id = parse_required_string(node, "channel-id", path + ".channel-id");
    auto conversion_id = parse_required_string(node, "conversion-id", path + ".conversion-id");
    auto display_type = parse_required_enum<CardDisplayType>(node, "display-type", path + ".display-type",
                                                             {{"numeric", CardDisplayType::Numeric},
                                                              {"sparkline", CardDisplayType::Sparkline},
                                                              {"horizontal-gauge", CardDisplayType::HorizontalGauge}});
    auto order = parse_required_u32(node, "order", path + ".order");
    if (!id)
    {
        return std::unexpected(id.error());
    }
    if (!channel_id)
    {
        return std::unexpected(channel_id.error());
    }
    if (!conversion_id)
    {
        return std::unexpected(conversion_id.error());
    }
    if (!display_type)
    {
        return std::unexpected(display_type.error());
    }
    if (!order)
    {
        return std::unexpected(order.error());
    }

    const bool has_minimum = node.attribute("gauge-min");
    const bool has_maximum = node.attribute("gauge-max");
    const bool has_step = node.attribute("gauge-step");
    std::optional<GaugeBoundsOverride> gauge_bounds;
    if (has_minimum || has_maximum || has_step)
    {
        if (!has_minimum || !has_maximum || !has_step)
        {
            return invalid(path + ".gauge", "gauge-min, gauge-max, and gauge-step must be specified together");
        }
        auto minimum = parse_required_double(node, "gauge-min", path + ".gauge-min");
        auto maximum = parse_required_double(node, "gauge-max", path + ".gauge-max");
        auto step = parse_required_double(node, "gauge-step", path + ".gauge-step");
        if (!minimum)
        {
            return std::unexpected(minimum.error());
        }
        if (!maximum)
        {
            return std::unexpected(maximum.error());
        }
        if (!step)
        {
            return std::unexpected(step.error());
        }
        gauge_bounds = GaugeBoundsOverride{*minimum, *maximum, *step};
    }

    std::optional<std::uint16_t> history;
    if (node.attribute("sparkline-history-seconds"))
    {
        auto parsed = parse_required_unsigned<std::uint16_t>(node, "sparkline-history-seconds",
                                                             path + ".sparkline-history-seconds");
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }
        history = *parsed;
    }

    return DashboardCard{std::move(*id),
                         std::move(*channel_id),
                         std::move(*conversion_id),
                         *display_type,
                         parse_optional_string(node, "title"),
                         *order,
                         gauge_bounds,
                         history};
}

Result<DashboardDocument> decode_v1(pugi::xml_node root)
{
    if (Status status = reject_unknown_attributes(root, {"format-version"}, "document.root"); !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(root, {"metadata", "connection", "channels", "cards"}, "document.root");
        !status)
    {
        return std::unexpected(status.error());
    }

    auto metadata_node = require_single_child(root, "metadata", "metadata");
    auto connection_node = require_single_child(root, "connection", "connection");
    auto channels_node = require_single_child(root, "channels", "channels");
    auto cards_node = require_single_child(root, "cards", "cards");
    if (!metadata_node)
    {
        return std::unexpected(metadata_node.error());
    }
    if (!connection_node)
    {
        return std::unexpected(connection_node.error());
    }
    if (!channels_node)
    {
        return std::unexpected(channels_node.error());
    }
    if (!cards_node)
    {
        return std::unexpected(cards_node.error());
    }

    if (Status status = reject_unknown_attributes(*metadata_node, {"name", "description"}, "metadata"); !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(*metadata_node, {}, "metadata"); !status)
    {
        return std::unexpected(status.error());
    }
    auto name = parse_required_string(*metadata_node, "name", "metadata.name");
    if (!name)
    {
        return std::unexpected(name.error());
    }

    constexpr std::string_view connection_path = "connection";
    if (Status status = reject_unknown_attributes(*connection_node,
                                                  {"protocol", "transport", "bitrate", "identifier-width", "request-id",
                                                   "reply-id", "stream-instance", "sampling-interval-ms"},
                                                  connection_path);
        !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(*connection_node, {"retry", "preferred-adapter"}, connection_path);
        !status)
    {
        return std::unexpected(status.error());
    }
    auto retry_node = require_single_child(*connection_node, "retry", "connection.retry");
    auto adapter_node = optional_single_child(*connection_node, "preferred-adapter", "connection.preferred-adapter");
    if (!retry_node)
    {
        return std::unexpected(retry_node.error());
    }
    if (!adapter_node)
    {
        return std::unexpected(adapter_node.error());
    }

    auto protocol = parse_required_enum<DashboardProtocol>(*connection_node, "protocol", "connection.protocol",
                                                           {{"cdbg", DashboardProtocol::Cdbg}});
    auto transport = parse_required_enum<DashboardTransport>(*connection_node, "transport", "connection.transport",
                                                             {{"raw-can", DashboardTransport::RawCan}});
    auto bitrate = parse_required_u32(*connection_node, "bitrate", "connection.bitrate");
    auto identifier_width = parse_required_enum<CanIdentifierWidth>(
        *connection_node, "identifier-width", "connection.identifier-width",
        {{"11", CanIdentifierWidth::Standard}, {"29", CanIdentifierWidth::Extended}});
    auto request_id = parse_required_u32(*connection_node, "request-id", "connection.request-id", 16);
    auto reply_id = parse_required_u32(*connection_node, "reply-id", "connection.reply-id", 16);
    auto stream_instance =
        parse_required_unsigned<std::uint8_t>(*connection_node, "stream-instance", "connection.stream-instance");
    auto sampling_interval =
        parse_required_u32(*connection_node, "sampling-interval-ms", "connection.sampling-interval-ms");
    auto retry = decode_retry(*retry_node);
    if (!protocol)
    {
        return std::unexpected(protocol.error());
    }
    if (!transport)
    {
        return std::unexpected(transport.error());
    }
    if (!bitrate)
    {
        return std::unexpected(bitrate.error());
    }
    if (!identifier_width)
    {
        return std::unexpected(identifier_width.error());
    }
    if (!request_id)
    {
        return std::unexpected(request_id.error());
    }
    if (!reply_id)
    {
        return std::unexpected(reply_id.error());
    }
    if (!stream_instance)
    {
        return std::unexpected(stream_instance.error());
    }
    if (!sampling_interval)
    {
        return std::unexpected(sampling_interval.error());
    }
    if (!retry)
    {
        return std::unexpected(retry.error());
    }

    std::optional<PreferredAdapter> adapter;
    if (adapter_node->has_value())
    {
        auto decoded = decode_adapter(**adapter_node);
        if (!decoded)
        {
            return std::unexpected(decoded.error());
        }
        adapter = std::move(*decoded);
    }

    if (Status status = reject_unknown_attributes(*channels_node, {}, "channels"); !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(*channels_node, {"channel"}, "channels"); !status)
    {
        return std::unexpected(status.error());
    }
    std::vector<DashboardChannel> channels;
    std::size_t channel_index = 0;
    for (const pugi::xml_node channel_node : channels_node->children("channel"))
    {
        auto channel = decode_channel(channel_node, channel_index);
        if (!channel)
        {
            return std::unexpected(channel.error());
        }
        channels.push_back(std::move(*channel));
        ++channel_index;
    }

    if (Status status = reject_unknown_attributes(*cards_node, {}, "cards"); !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(*cards_node, {"card"}, "cards"); !status)
    {
        return std::unexpected(status.error());
    }
    std::vector<DashboardCard> cards;
    std::size_t card_index = 0;
    for (const pugi::xml_node card_node : cards_node->children("card"))
    {
        auto card = decode_card(card_node, card_index);
        if (!card)
        {
            return std::unexpected(card.error());
        }
        cards.push_back(std::move(*card));
        ++card_index;
    }

    DashboardDocument candidate{
        .metadata = DocumentMetadata{1, std::move(*name), parse_optional_string(*metadata_node, "description")},
        .connection = CdbgConnectionProfile{*protocol, *transport, *bitrate, *identifier_width, *request_id, *reply_id,
                                            *stream_instance, *sampling_interval, *retry, std::move(adapter)},
        .channels = std::move(channels),
        .cards = std::move(cards),
    };
    if (Status status = validate_dashboard_document_xml_strings(candidate); !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = validate_dashboard_document(candidate); !status)
    {
        return std::unexpected(status.error());
    }
    return candidate;
}

template <typename Integer> std::string decimal(Integer value)
{
    char buffer[32];
    const auto [end, error] = std::to_chars(std::begin(buffer), std::end(buffer), value, 10);
    if (error != std::errc{})
    {
        return {};
    }
    return {buffer, end};
}

std::string hexadecimal(std::uint32_t value)
{
    char buffer[16];
    const auto [end, error] = std::to_chars(std::begin(buffer), std::end(buffer), value, 16);
    if (error != std::errc{})
    {
        return {};
    }
    return "0x" + std::string(buffer, end);
}

std::string floating(double value)
{
    char buffer[64];
    const auto [end, error] = std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (error != std::errc{})
    {
        return {};
    }
    return {buffer, end};
}

void append_attribute(pugi::xml_node node, const char *name, std::string_view value)
{
    node.append_attribute(name).set_value(value.data(), value.size());
}

std::string_view spelling(AdapterKind value)
{
    switch (value)
    {
    case AdapterKind::J2534:
        return "j2534";
    case AdapterKind::SocketCan:
        return "socketcan";
    }
    return {};
}

std::string_view spelling(CardDisplayType value)
{
    switch (value)
    {
    case CardDisplayType::Numeric:
        return "numeric";
    case CardDisplayType::Sparkline:
        return "sparkline";
    case CardDisplayType::HorizontalGauge:
        return "horizontal-gauge";
    }
    return {};
}

void remove_empty_element_spaces(std::string& text)
{
    bool in_tag = false;
    char quote = '\0';
    for (std::size_t index = 0; index + 2 < text.size(); ++index)
    {
        const char current = text[index];
        if (!in_tag)
        {
            in_tag = current == '<';
            continue;
        }
        if (quote != '\0')
        {
            if (current == quote)
            {
                quote = '\0';
            }
            continue;
        }
        if (current == '\'' || current == '"')
        {
            quote = current;
            continue;
        }
        if (current == '>')
        {
            in_tag = false;
            continue;
        }
        if (current == ' ' && text[index + 1] == '/' && text[index + 2] == '>')
        {
            text.erase(index, 1);
            --index;
        }
    }
}
} // namespace

Result<DashboardDocument> decode_dashboard_document(bytes::ByteView xml)
{
    if (Status status = validate_xml_input(xml, "document"); !status)
    {
        return std::unexpected(status.error());
    }
    pugi::xml_document tree;
    const auto parsed = tree.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!parsed)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("document: {} at offset {}", parsed.description(), parsed.offset));
    }
    std::size_t root_count = 0;
    for (const pugi::xml_node node : tree.children())
    {
        if (node.type() == pugi::node_element)
        {
            ++root_count;
        }
    }
    if (root_count != 1)
    {
        return fail(ErrorKind::InvalidConfig, "document.root: expected exactly one document element");
    }
    const pugi::xml_node root = tree.document_element();
    if (std::string_view(root.name()) != "omnihaste-dashboard")
    {
        return fail(ErrorKind::InvalidConfig, "document.root: expected <omnihaste-dashboard>");
    }
    const auto version = parse_format_version(root);
    if (!version)
    {
        return std::unexpected(version.error());
    }
    if (!is_format_version_one(*version))
    {
        return fail(ErrorKind::Unsupported, "metadata.format-version: unsupported version " + *version);
    }
    return decode_v1(root);
}

Result<std::vector<std::uint8_t>> encode_dashboard_document(const DashboardDocument& document)
{
    if (Status status = validate_dashboard_document_xml_strings(document); !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = validate_dashboard_document(document); !status)
    {
        return std::unexpected(status.error());
    }

    pugi::xml_document tree;
    pugi::xml_node declaration = tree.append_child(pugi::node_declaration);
    append_attribute(declaration, "version", "1.0");
    append_attribute(declaration, "encoding", "utf-8");

    pugi::xml_node root = tree.append_child("omnihaste-dashboard");
    append_attribute(root, "format-version", decimal(document.metadata.format_version));

    pugi::xml_node metadata = root.append_child("metadata");
    append_attribute(metadata, "name", document.metadata.name);
    if (document.metadata.description)
    {
        append_attribute(metadata, "description", *document.metadata.description);
    }

    pugi::xml_node connection = root.append_child("connection");
    append_attribute(connection, "protocol", "cdbg");
    append_attribute(connection, "transport", "raw-can");
    append_attribute(connection, "bitrate", decimal(document.connection.bitrate));
    append_attribute(connection, "identifier-width",
                     decimal(static_cast<std::uint8_t>(document.connection.identifier_width)));
    append_attribute(connection, "request-id", hexadecimal(document.connection.request_id));
    append_attribute(connection, "reply-id", hexadecimal(document.connection.reply_id));
    append_attribute(connection, "stream-instance", decimal(document.connection.stream_instance));
    append_attribute(connection, "sampling-interval-ms", decimal(document.connection.sampling_interval_ms));

    pugi::xml_node retry = connection.append_child("retry");
    append_attribute(retry, "poll-timeout-ms", decimal(document.connection.retry.poll_timeout_ms));
    append_attribute(retry, "silence-threshold", decimal(document.connection.retry.silence_threshold));
    append_attribute(retry, "reconnect-attempts", decimal(document.connection.retry.reconnect_attempts));
    append_attribute(retry, "reconnect-period-ms", decimal(document.connection.retry.reconnect_period_ms));

    if (document.connection.preferred_adapter)
    {
        pugi::xml_node adapter = connection.append_child("preferred-adapter");
        append_attribute(adapter, "kind", spelling(document.connection.preferred_adapter->kind));
        append_attribute(adapter, "vendor", document.connection.preferred_adapter->vendor);
        append_attribute(adapter, "display-name", document.connection.preferred_adapter->display_name);
    }

    pugi::xml_node channels = root.append_child("channels");
    for (const DashboardChannel& source : document.channels)
    {
        pugi::xml_node channel = channels.append_child("channel");
        append_attribute(channel, "id", source.id);
        append_attribute(channel, "name", source.name);
        append_attribute(channel, "description", source.description);
        append_attribute(channel, "address", hexadecimal(source.address));
        append_attribute(channel, "length", decimal(source.length));
        append_attribute(channel, "raw-assembly", "unsigned-integer-decimal");
        for (const DashboardConversion& source_conversion : source.conversions)
        {
            pugi::xml_node conversion = channel.append_child("conversion");
            append_attribute(conversion, "id", source_conversion.id);
            append_attribute(conversion, "expression", source_conversion.expression);
            append_attribute(conversion, "unit", source_conversion.unit);
            append_attribute(conversion, "precision", decimal(source_conversion.precision));
            append_attribute(conversion, "gauge-min", floating(source_conversion.gauge_min));
            append_attribute(conversion, "gauge-max", floating(source_conversion.gauge_max));
            append_attribute(conversion, "gauge-step", floating(source_conversion.gauge_step));
        }
    }

    pugi::xml_node cards = root.append_child("cards");
    std::vector<const DashboardCard *> ordered_cards;
    ordered_cards.reserve(document.cards.size());
    for (const DashboardCard& card : document.cards)
    {
        ordered_cards.push_back(&card);
    }
    std::ranges::sort(ordered_cards, {}, [](const DashboardCard *card) { return card->order; });
    for (const DashboardCard *source : ordered_cards)
    {
        pugi::xml_node card = cards.append_child("card");
        append_attribute(card, "id", source->id);
        append_attribute(card, "channel-id", source->channel_id);
        append_attribute(card, "conversion-id", source->conversion_id);
        append_attribute(card, "display-type", spelling(source->display_type));
        if (source->title)
        {
            append_attribute(card, "title", *source->title);
        }
        append_attribute(card, "order", decimal(source->order));
        if (source->gauge_bounds)
        {
            append_attribute(card, "gauge-min", floating(source->gauge_bounds->minimum));
            append_attribute(card, "gauge-max", floating(source->gauge_bounds->maximum));
            append_attribute(card, "gauge-step", floating(source->gauge_bounds->step));
        }
        if (source->sparkline_history_seconds)
        {
            append_attribute(card, "sparkline-history-seconds", decimal(*source->sparkline_history_seconds));
        }
    }

    std::ostringstream output;
    tree.save(output, "    ", pugi::format_default, pugi::encoding_utf8);
    std::string text = output.str();
    remove_empty_element_spaces(text);
    while (text.size() >= 2 && text.ends_with("\n\n"))
    {
        text.pop_back();
    }
    if (!text.ends_with('\n'))
    {
        text.push_back('\n');
    }
    return std::vector<std::uint8_t>(text.begin(), text.end());
}
} // namespace fastecu::dashboard
