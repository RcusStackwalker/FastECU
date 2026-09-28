#include "src/backend/logging/logger_model.h"

#include <charconv>
#include <cstdint>
#include <ranges>
#include <utility>

#include "src/backend/logging/logger_conf.h"

namespace fastecu::logging
{
namespace
{
LoggerIdentity identity(std::string_view protocol, std::string_view id)
{
    return {std::string(protocol), std::string(id)};
}
unsigned decimal(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\n\r\v\f");
    if (first == std::string_view::npos)
    {
        return 0;
    }
    text = text.substr(first, text.find_last_not_of(" \t\n\r\v\f") - first + 1);
    if (text.starts_with('+'))
    {
        text.remove_prefix(1);
    }
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? value : 0;
}
bool supported(const std::map<LoggerIdentity, bool>& flags, std::string_view protocol, std::string_view id)
{
    const auto it = flags.find(identity(protocol, id));
    return it != flags.end() && it->second;
}
void update(std::map<LoggerIdentity, bool>& flags, std::string_view protocol, std::string_view id, bool value)
{
    if (const auto it = flags.find(identity(protocol, id)); it != flags.end())
    {
        it->second = value;
    }
}
} // namespace

bool LoggerModel::install_definition(LoggerDefinition definition)
{
    if (installed_)
    {
        return false;
    }
    installed_ = true;
    definition_ = std::move(definition);
    selection_ = initial_selection(definition_);
    for (const auto& parameter : definition_.parameters)
    {
        parameter_support_.emplace(identity(parameter.protocol, parameter.id), parameter.enabled);
    }
    for (const auto& item : definition_.switches)
    {
        switch_support_.emplace(identity(item.protocol, item.id), item.enabled);
    }
    return true;
}
const LoggerDefinition& LoggerModel::definition() const
{
    return definition_;
}
const LoggerSelection& LoggerModel::selection() const
{
    return selection_;
}
void LoggerModel::set_selection(LoggerSelection selection)
{
    selection_ = std::move(selection);
}
const LoggerParameter *LoggerModel::parameter(std::string_view protocol, std::string_view id) const
{
    const auto it = std::ranges::find_if(definition_.parameters,
                                         [&](const auto& p) { return p.protocol == protocol && p.id == id; });
    return it == definition_.parameters.end() ? nullptr : &*it;
}
const LoggerSwitch *LoggerModel::switch_definition(std::string_view protocol, std::string_view id) const
{
    const auto it =
        std::ranges::find_if(definition_.switches, [&](const auto& p) { return p.protocol == protocol && p.id == id; });
    return it == definition_.switches.end() ? nullptr : &*it;
}
bool LoggerModel::parameter_supported(std::string_view protocol, std::string_view id) const
{
    return supported(parameter_support_, protocol, id);
}
bool LoggerModel::switch_supported(std::string_view protocol, std::string_view id) const
{
    return supported(switch_support_, protocol, id);
}
void LoggerModel::set_parameter_supported(std::string_view protocol, std::string_view id, bool value)
{
    update(parameter_support_, protocol, id, value);
}
void LoggerModel::set_switch_supported(std::string_view protocol, std::string_view id, bool value)
{
    update(switch_support_, protocol, id, value);
}
void LoggerModel::apply_capabilities(std::string_view protocol, bytes::ByteView capabilities)
{
    for (const auto& p : definition_.parameters)
    {
        if (p.protocol != protocol)
        {
            continue;
        }
        const auto index = static_cast<std::uint16_t>(decimal(p.ecu_byte_index));
        const auto bit = static_cast<std::uint8_t>(decimal(p.ecu_bit));
        set_parameter_supported(protocol, p.id,
                                p.ecu_byte_index != "No byte index" && index < capabilities.size() && bit < 8 &&
                                    (capabilities[index] & (1U << bit)) != 0);
    }
    for (const auto& p : definition_.switches)
    {
        const auto index = static_cast<std::uint16_t>(decimal(p.ecu_byte_index));
        const auto bit = static_cast<std::uint8_t>(decimal(p.ecu_bit));
        if (p.protocol == protocol && index < capabilities.size())
        {
            set_switch_supported(protocol, p.id, bit < 8 && (capabilities[index] & (1U << bit)) != 0);
        }
    }
}
LoggerSelection LoggerModel::default_selection() const
{
    LoggerSelection result;
    if (!definition_.parameters.empty())
    {
        result.protocol = definition_.parameters.front().protocol;
    }
    for (const auto& p : definition_.parameters)
    {
        if (!parameter_supported(p.protocol, p.id))
        {
            continue;
        }
        if (result.gauge_ids.size() < 15)
        {
            result.gauge_ids.push_back(p.id);
        }
        if (result.lower_panel_ids.size() < 12)
        {
            result.lower_panel_ids.push_back(p.id);
        }
    }
    for (const auto& p : definition_.switches)
    {
        if (switch_supported(p.protocol, p.id) && result.switch_ids.size() < 20)
        {
            result.switch_ids.push_back(p.id);
        }
    }
    return result;
}
} // namespace fastecu::logging
