#include "src/backend/logging/logger_definition_parser.h"

#include <format>
#include <string>

#include <pugixml.hpp>

namespace fastecu::logging
{
namespace
{

std::string attribute_or(pugi::xml_node node, const char *name, const char *fallback)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    return attribute ? attribute.value() : fallback;
}

Conversion parse_conversion(pugi::xml_node node)
{
    return Conversion{
        .units = attribute_or(node, "units", "#"),
        .expr = attribute_or(node, "expr", "x"),
        .format = attribute_or(node, "format", "0.00"),
        .gauge_min = attribute_or(node, "gauge_min", "No gauge_min"),
        .gauge_max = attribute_or(node, "gauge_max", "No gauge_max"),
        .gauge_step = attribute_or(node, "gauge_step", "No gauge_step"),
    };
}

LoggerParameter parse_parameter(pugi::xml_node node, std::string_view protocol)
{
    LoggerParameter parameter{
        .protocol = std::string(protocol),
        .id = attribute_or(node, "id", "No id"),
        .name = attribute_or(node, "name", "No name"),
        .description = attribute_or(node, "desc", "No desc"),
        .address = {},
        .length = {},
        .ecu_byte_index = attribute_or(node, "ecubyteindex", "No byte index"),
        .ecu_bit = attribute_or(node, "ecubit", "No ecu bit"),
        .target = attribute_or(node, "target", "No target"),
        .enabled = attribute_or(node, "enabled", "0") == "1",
        .conversions = {},
    };

    if (const pugi::xml_node address = node.child("address"))
    {
        parameter.address = address.child_value();
        parameter.length = attribute_or(node, "length", "1");
    }
    for (pugi::xml_node conversion : node.child("conversions").children("conversion"))
    {
        parameter.conversions.push_back(parse_conversion(conversion));
    }
    return parameter;
}

LoggerSwitch parse_switch(pugi::xml_node node, std::string_view protocol)
{
    return LoggerSwitch{
        .protocol = std::string(protocol),
        .id = attribute_or(node, "id", "No id"),
        .name = attribute_or(node, "name", "No name"),
        .description = attribute_or(node, "desc", "No desc"),
        .address = attribute_or(node, "byte", "No address"),
        .ecu_byte_index = attribute_or(node, "ecubyteindex", "No ecu byte index"),
        .ecu_bit = attribute_or(node, "bit", "No ecu bit"),
        .target = attribute_or(node, "target", "No target"),
    };
}

} // namespace

Result<LoggerDefinition> parse_logger_definition(
    bytes::ByteView xml, std::string_view source)
{
    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size());
    if (!parsed)
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::format("{}: {} at offset {}", source, parsed.description(), parsed.offset));
    }

    const pugi::xml_node root = document.child("logger");
    if (!root)
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::format("{}: expected root element <logger>", source));
    }

    LoggerDefinition definition;
    for (pugi::xml_node protocol : root.child("protocols").children("protocol"))
    {
        const std::string protocol_id = attribute_or(protocol, "id", "No protocol id");
        for (pugi::xml_node parameter : protocol.child("parameters").children("parameter"))
        {
            definition.parameters.push_back(parse_parameter(parameter, protocol_id));
        }
        for (pugi::xml_node paramswitch : protocol.child("switches").children("switch"))
        {
            definition.switches.push_back(parse_switch(paramswitch, protocol_id));
        }
    }
    return definition;
}

} // namespace fastecu::logging
