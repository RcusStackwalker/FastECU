#pragma once

#include <optional>
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

struct LoggerAddressSpec
{
    std::string value;
    std::optional<std::string> length;
    std::optional<std::string> bit;
    bool operator==(const LoggerAddressSpec&) const = default;
};

// One <parameter>. Identity is (protocol, id); order follows the XML.
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
    std::vector<LoggerAddressSpec> address_specs;
    std::optional<std::string> declared_length;

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
    // XML defaults only. LoggerModel owns runtime support separately.
    bool enabled{false};

    std::string sample_bit;
    std::vector<LoggerAddressSpec> address_specs;

    bool operator==(const LoggerSwitch&) const = default;
};

struct LoggerProtocolDefinition
{
    std::string id;
    std::string dialect;
    bool operator==(const LoggerProtocolDefinition&) const = default;
};
struct LoggerDefinition
{
    std::vector<LoggerParameter> parameters;
    std::vector<LoggerSwitch> switches;
    std::vector<LoggerProtocolDefinition> protocols;

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
