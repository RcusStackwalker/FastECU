#include "src/backend/logging/logger_definition_parser.h"

#include <format>
#include <string>

#include <pugixml.hpp>

namespace fastecu::logging
{
namespace
{

std::string AttributeOr(pugi::xml_node node, const char *name, const char *fallback)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    return attribute ? attribute.value() : fallback;
}

Conversion ParseConversion(pugi::xml_node node)
{
    return Conversion{
        .units = AttributeOr(node, "units", "#"),
        .expr = AttributeOr(node, "expr", "x"),
        .format = AttributeOr(node, "format", "0.00"),
        .gauge_min = AttributeOr(node, "gauge_min", "No gauge_min"),
        .gauge_max = AttributeOr(node, "gauge_max", "No gauge_max"),
        .gauge_step = AttributeOr(node, "gauge_step", "No gauge_step"),
    };
}

LoggerParameter ParseParameter(pugi::xml_node node, std::string_view protocol)
{
    LoggerParameter parameter{
        .protocol = std::string(protocol),
        .id = AttributeOr(node, "id", "No id"),
        .name = AttributeOr(node, "name", "No name"),
        .description = AttributeOr(node, "desc", "No desc"),
        .address = {},
        .length = {},
        .ecu_byte_index = AttributeOr(node, "ecubyteindex", "No byte index"),
        .ecu_bit = AttributeOr(node, "ecubit", "No ecu bit"),
        .target = AttributeOr(node, "target", "No target"),
        .enabled = AttributeOr(node, "enabled", "0") == "1",
        .conversions = {},
    };

    if (const pugi::xml_node address = node.child("address"))
    {
        parameter.address = address.child_value();
        parameter.length = AttributeOr(node, "length", "1");
    }
    for (pugi::xml_node conversion : node.child("conversions").children("conversion"))
    {
        parameter.conversions.push_back(ParseConversion(conversion));
    }
    return parameter;
}

LoggerSwitch ParseSwitch(pugi::xml_node node, std::string_view protocol)
{
    return LoggerSwitch{
        .protocol = std::string(protocol),
        .id = AttributeOr(node, "id", "No id"),
        .name = AttributeOr(node, "name", "No name"),
        .description = AttributeOr(node, "desc", "No desc"),
        .address = AttributeOr(node, "byte", "No address"),
        .ecu_byte_index = AttributeOr(node, "ecubyteindex", "No ecu byte index"),
        .ecu_bit = AttributeOr(node, "bit", "No ecu bit"),
        .target = AttributeOr(node, "target", "No target"),
        // The definition XML has no switch enabled attribute; the legacy
        // parser always seeded log_switch_enabled with "0" for every switch
        // (file_actions.cpp:1261). Runtime capability populates this later.
        .enabled = false,
    };
}

} // namespace

Result<LoggerDefinition> ParseLoggerDefinition(bytes::ByteView xml, std::string_view source)
{
    pugi::xml_document document;
    if (const pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size()); !parsed)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{}: {} at offset {}", source, parsed.description(), parsed.offset));
    }

    const pugi::xml_node root = document.child("logger");
    if (!root)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{}: expected root element <logger>", source));
    }

    LoggerDefinition definition;
    for (pugi::xml_node protocol : root.child("protocols").children("protocol"))
    {
        const std::string protocol_id = AttributeOr(protocol, "id", "No protocol id");
        for (pugi::xml_node parameter : protocol.child("parameters").children("parameter"))
        {
            definition.parameters.push_back(ParseParameter(parameter, protocol_id));
        }
        for (pugi::xml_node paramswitch : protocol.child("switches").children("switch"))
        {
            definition.switches.push_back(ParseSwitch(paramswitch, protocol_id));
        }
    }
    return definition;
}

} // namespace fastecu::logging
