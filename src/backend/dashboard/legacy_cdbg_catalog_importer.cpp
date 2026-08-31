#include "src/backend/dashboard/legacy_cdbg_catalog_importer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"
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

bool contains(AllowedNames names, std::string_view candidate)
{
    return std::ranges::find(names, candidate) != names.end();
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

Result<pugi::xml_node> require_single_child(pugi::xml_node parent, const char *name, std::string path)
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
        return invalid(std::move(path), std::format("missing required <{}>", name));
    }
    if (count != 1)
    {
        return invalid(std::move(path), std::format("expected exactly one <{}>", name));
    }
    return found;
}

std::string attribute_or(pugi::xml_node node, const char *name, std::string_view fallback)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    return attribute ? std::string(attribute.value()) : std::string(fallback);
}

Result<std::uint32_t> parse_hex_address(std::string_view text, std::string path)
{
    if (!text.starts_with("0x") || text.size() == 2)
    {
        return invalid(std::move(path), "expected hexadecimal text beginning with '0x'");
    }
    text.remove_prefix(2);
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc{} || end != text.data() + text.size() || value > std::numeric_limits<std::uint32_t>::max())
    {
        return invalid(std::move(path), "invalid or out-of-range hexadecimal address");
    }
    return static_cast<std::uint32_t>(value);
}

Result<std::uint8_t> parse_length(pugi::xml_node parameter, std::string path)
{
    const std::string text = attribute_or(parameter, "length", "1");
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (error != std::errc{} || end != text.data() + text.size() || value > std::numeric_limits<std::uint8_t>::max())
    {
        return invalid(std::move(path), "invalid or out-of-range decimal length");
    }
    if (value != 1 && value != 2 && value != 4)
    {
        return invalid(std::move(path), "must be 1, 2, or 4 bytes");
    }
    return static_cast<std::uint8_t>(value);
}

Result<double> parse_required_double(pugi::xml_node node, const char *attribute_name, std::string path)
{
    const pugi::xml_attribute attribute = node.attribute(attribute_name);
    if (!attribute)
    {
        return invalid(std::move(path), std::format("missing required attribute '{}'", attribute_name));
    }
    const std::string_view text = attribute.value();
    double value = 0.0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
    {
        return invalid(std::move(path), "expected a finite floating-point number");
    }
    return value;
}

Result<std::uint8_t> parse_precision(pugi::xml_node conversion, std::string path)
{
    const std::string format = attribute_or(conversion, "format", "0.00");
    if (format == "0")
    {
        return 0;
    }
    if (format.size() < 3 || !format.starts_with("0.") ||
        !std::ranges::all_of(std::string_view(format).substr(2), [](char digit) { return digit == '0'; }))
    {
        return invalid(std::move(path), "expected '0' or '0.' followed by one or more zeroes");
    }
    const std::size_t precision = format.size() - 2;
    if (precision > std::numeric_limits<std::uint8_t>::max())
    {
        return invalid(std::move(path), "display precision is too large");
    }
    return static_cast<std::uint8_t>(precision);
}

std::string channel_path(pugi::xml_node parameter, std::size_t index)
{
    const pugi::xml_attribute id = parameter.attribute("id");
    if (!id || std::string_view(id.value()).empty())
    {
        return "channels[" + std::to_string(index) + "]";
    }
    return "channels[" + std::string(id.value()) + "]";
}

Result<DashboardConversion> import_conversion(pugi::xml_node conversion, std::size_t index,
                                              const std::string& parent_path)
{
    const std::string path = parent_path + ".conversions[conversion-" + std::to_string(index + 1) + "]";
    if (Status status = reject_unknown_attributes(
            conversion, {"units", "expr", "format", "gauge_min", "gauge_max", "gauge_step"}, path);
        !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(conversion, {}, path); !status)
    {
        return std::unexpected(status.error());
    }

    auto precision = parse_precision(conversion, path + ".format");
    if (!precision)
    {
        return std::unexpected(precision.error());
    }
    auto gauge_min = parse_required_double(conversion, "gauge_min", path + ".gauge-min");
    if (!gauge_min)
    {
        return std::unexpected(gauge_min.error());
    }
    auto gauge_max = parse_required_double(conversion, "gauge_max", path + ".gauge-max");
    if (!gauge_max)
    {
        return std::unexpected(gauge_max.error());
    }
    auto gauge_step = parse_required_double(conversion, "gauge_step", path + ".gauge-step");
    if (!gauge_step)
    {
        return std::unexpected(gauge_step.error());
    }

    return DashboardConversion{
        .id = "conversion-" + std::to_string(index + 1),
        .expression = attribute_or(conversion, "expr", "x"),
        .unit = attribute_or(conversion, "units", "#"),
        .precision = *precision,
        .gauge_min = *gauge_min,
        .gauge_max = *gauge_max,
        .gauge_step = *gauge_step,
    };
}

Result<DashboardChannel> import_parameter(pugi::xml_node parameter, std::size_t index)
{
    const std::string path = channel_path(parameter, index);
    if (Status status = reject_unknown_attributes(parameter, {"id", "name", "desc", "length", "enabled"}, path);
        !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(parameter, {"address", "conversions"}, path); !status)
    {
        return std::unexpected(status.error());
    }

    const pugi::xml_attribute id_attribute = parameter.attribute("id");
    if (!id_attribute || std::string_view(id_attribute.value()).empty() ||
        std::string_view(id_attribute.value()) == "No id")
    {
        return invalid(path + ".id", "must be present, non-empty, and not the legacy 'No id' sentinel");
    }

    auto length = parse_length(parameter, path + ".length");
    if (!length)
    {
        return std::unexpected(length.error());
    }
    auto address_node = require_single_child(parameter, "address", path + ".address");
    if (!address_node)
    {
        return std::unexpected(address_node.error());
    }
    if (Status status = reject_unknown_attributes(*address_node, {}, path + ".address"); !status)
    {
        return std::unexpected(status.error());
    }
    std::string address_text;
    for (const pugi::xml_node child : address_node->children())
    {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata)
        {
            address_text.append(child.value());
            continue;
        }
        if (child.type() == pugi::node_element)
        {
            return invalid(path + ".address", std::format("unknown element <{}>", child.name()));
        }
    }
    auto address = parse_hex_address(address_text, path + ".address");
    if (!address)
    {
        return std::unexpected(address.error());
    }

    auto conversions_node = require_single_child(parameter, "conversions", path + ".conversions");
    if (!conversions_node)
    {
        return std::unexpected(conversions_node.error());
    }
    if (Status status = reject_unknown_attributes(*conversions_node, {}, path + ".conversions"); !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(*conversions_node, {"conversion"}, path + ".conversions"); !status)
    {
        return std::unexpected(status.error());
    }

    std::vector<DashboardConversion> conversions;
    for (const pugi::xml_node conversion : conversions_node->children("conversion"))
    {
        auto imported = import_conversion(conversion, conversions.size(), path);
        if (!imported)
        {
            return std::unexpected(imported.error());
        }
        conversions.push_back(std::move(*imported));
    }
    if (conversions.empty())
    {
        return invalid(path + ".conversions", "must contain at least one <conversion>");
    }

    return DashboardChannel{
        .id = id_attribute.value(),
        .name = attribute_or(parameter, "name", "No name"),
        .description = attribute_or(parameter, "desc", "No desc"),
        .address = *address,
        .length = *length,
        .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
        .conversions = std::move(conversions),
    };
}
} // namespace

Result<DashboardDocument> import_legacy_cdbg_catalog(bytes::ByteView xml, const LegacyCdbgImportDefaults& defaults)
{
    if (Status status = validate_xml_input(xml, "legacy-cdbg"); !status)
    {
        return std::unexpected(status.error());
    }
    pugi::xml_document tree;
    const pugi::xml_parse_result parsed =
        tree.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!parsed)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("legacy-cdbg: {} at offset {}", parsed.description(), parsed.offset));
    }

    std::size_t root_count = 0;
    for (const pugi::xml_node child : tree.children())
    {
        if (child.type() == pugi::node_element)
        {
            ++root_count;
        }
    }
    const pugi::xml_node root = tree.document_element();
    if (root_count != 1 || std::string_view(root.name()) != "logger")
    {
        return invalid("legacy-cdbg.root", "expected exactly one <logger> document element");
    }

    auto protocols = require_single_child(root, "protocols", "legacy-cdbg.protocols");
    if (!protocols)
    {
        return std::unexpected(protocols.error());
    }

    pugi::xml_node selected_protocol;
    std::size_t cdbg_count = 0;
    for (const pugi::xml_node protocol : protocols->children("protocol"))
    {
        if (std::string_view(protocol.attribute("id").value()) == "CDBG")
        {
            selected_protocol = protocol;
            ++cdbg_count;
        }
    }
    if (cdbg_count != 1)
    {
        return invalid("legacy-cdbg.protocol", "expected exactly one direct <protocol id='CDBG'>");
    }

    constexpr std::string_view protocol_path = "legacy-cdbg.protocol";
    if (Status status = reject_unknown_attributes(selected_protocol, {"id"}, protocol_path); !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(selected_protocol, {"parameters", "switches"}, protocol_path); !status)
    {
        return std::unexpected(status.error());
    }
    auto parameters = require_single_child(selected_protocol, "parameters", "legacy-cdbg.protocol.parameters");
    if (!parameters)
    {
        return std::unexpected(parameters.error());
    }
    if (Status status = reject_unknown_attributes(*parameters, {}, "legacy-cdbg.protocol.parameters"); !status)
    {
        return std::unexpected(status.error());
    }
    if (Status status = reject_unknown_children(*parameters, {"parameter"}, "legacy-cdbg.protocol.parameters"); !status)
    {
        return std::unexpected(status.error());
    }

    std::vector<DashboardChannel> channels;
    std::unordered_set<std::string> channel_ids;
    for (const pugi::xml_node parameter : parameters->children("parameter"))
    {
        auto channel = import_parameter(parameter, channels.size());
        if (!channel)
        {
            return std::unexpected(channel.error());
        }
        if (!channel_ids.insert(channel->id).second)
        {
            return invalid("channels[" + channel->id + "].id", "must be unique");
        }
        channels.push_back(std::move(*channel));
    }
    if (channels.empty())
    {
        return invalid("legacy-cdbg.protocol.parameters", "must contain at least one <parameter>");
    }

    DashboardDocument candidate{
        .metadata = {.format_version = 1, .name = defaults.document_name},
        .connection =
            {
                .protocol = DashboardProtocol::Cdbg,
                .transport = DashboardTransport::RawCan,
                .bitrate = defaults.bitrate,
                .identifier_width = defaults.identifier_width,
                .request_id = MitsuColtCanCdbg::kRequestCanId,
                .reply_id = MitsuColtCanCdbg::kReplyCanId,
                .stream_instance = defaults.stream_instance,
                .sampling_interval_ms = defaults.sampling_interval_ms,
                .retry = defaults.retry,
                .preferred_adapter = std::nullopt,
            },
        .channels = std::move(channels),
        .cards = {},
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
} // namespace fastecu::dashboard
