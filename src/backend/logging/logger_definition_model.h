#pragma once

#include <string>
#include <vector>

namespace fastecu::logging
{

// One <conversion> element. All fields stay strings: the expression is
// evaluated downstream by the expression evaluator, and the gauge bounds are
// presentation values the UI parses on demand.
struct Conversion
{
    std::string units;
    std::string expr;
    std::string format;
    std::string gauge_min;
    std::string gauge_max;
    std::string gauge_step;

    bool operator==(const Conversion&) const = default;
};

// One <parameter>. `protocol` stays a field rather than becoming a grouping
// key because every consumer filters with `log_value_protocol.at(j) ==
// protocol`; regrouping is step-6 work.
struct LoggerParameter
{
    std::string protocol;
    std::string id;
    std::string name;
    std::string description;
    std::string address;
    std::string length;
    std::string ecu_byte_index;
    std::string ecu_bit;
    std::string target;
    bool enabled{false};
    std::vector<Conversion> conversions;

    bool operator==(const LoggerParameter&) const = default;
};

struct LoggerSwitch
{
    std::string protocol;
    std::string id;
    std::string name;
    std::string description;
    std::string address;
    std::string ecu_byte_index;
    std::string ecu_bit;
    std::string target;
    // Always false out of the parser: the definition XML carries no switch
    // "enabled" attribute (file_actions.cpp:1261 hardcodes "0" when building
    // the legacy log_switch_enabled list). Populated from the ECU's runtime
    // capability response by Task 8, not by parse_logger_definition.
    bool enabled{false};

    bool operator==(const LoggerSwitch&) const = default;
};

struct LoggerDefinition
{
    std::vector<LoggerParameter> parameters;
    std::vector<LoggerSwitch> switches;

    bool operator==(const LoggerDefinition&) const = default;
};

// The user's per-ECU choice of what to display. Distinct from the definition:
// see the ownership table in the 5d-5 design doc.
struct LoggerSelection
{
    std::string protocol;
    std::vector<std::string> gauge_ids;
    std::vector<std::string> lower_panel_ids;
    std::vector<std::string> switch_ids;

    bool operator==(const LoggerSelection&) const = default;
};

} // namespace fastecu::logging
