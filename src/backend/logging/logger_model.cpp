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
LoggerIdentity Identity(std::string_view protocol, std::string_view id)
{
    return {std::string(protocol), std::string(id)};
}
unsigned Decimal(std::string_view text)
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
bool Supported(const std::map<LoggerIdentity, bool>& flags, std::string_view protocol, std::string_view id)
{
    const auto it = flags.find(Identity(protocol, id));
    return it != flags.end() && it->second;
}
void Update(std::map<LoggerIdentity, bool>& flags, std::string_view protocol, std::string_view id, bool value)
{
    if (const auto it = flags.find(Identity(protocol, id)); it != flags.end())
    {
        it->second = value;
    }
}
} // namespace

bool LoggerModel::InstallDefinition(LoggerDefinition definition)
{
    if (installed_)
    {
        return false;
    }
    installed_ = true;
    definition_ = std::move(definition);
    selection_ = InitialSelection(definition_);
    for (const auto& parameter : definition_.parameters)
    {
        parameter_support_.emplace(Identity(parameter.protocol, parameter.id), parameter.enabled);
    }
    for (const auto& item : definition_.switches)
    {
        switch_support_.emplace(Identity(item.protocol, item.id), item.enabled);
    }
    return true;
}
const LoggerDefinition& LoggerModel::Definition() const
{
    return definition_;
}
const LoggerSelection& LoggerModel::Selection() const
{
    return selection_;
}
void LoggerModel::SetSelection(LoggerSelection selection)
{
    selection_ = std::move(selection);
}
const LoggerParameter *LoggerModel::Parameter(std::string_view protocol, std::string_view id) const
{
    const auto it = std::ranges::find_if(definition_.parameters,
                                         [&](const auto& p) { return p.protocol == protocol && p.id == id; });
    return it == definition_.parameters.end() ? nullptr : &*it;
}
const LoggerSwitch *LoggerModel::SwitchDefinition(std::string_view protocol, std::string_view id) const
{
    const auto it =
        std::ranges::find_if(definition_.switches, [&](const auto& p) { return p.protocol == protocol && p.id == id; });
    return it == definition_.switches.end() ? nullptr : &*it;
}
bool LoggerModel::ParameterSupported(std::string_view protocol, std::string_view id) const
{
    return Supported(parameter_support_, protocol, id);
}
bool LoggerModel::SwitchSupported(std::string_view protocol, std::string_view id) const
{
    return Supported(switch_support_, protocol, id);
}
void LoggerModel::SetParameterSupported(std::string_view protocol, std::string_view id, bool value)
{
    Update(parameter_support_, protocol, id, value);
}
void LoggerModel::SetSwitchSupported(std::string_view protocol, std::string_view id, bool value)
{
    Update(switch_support_, protocol, id, value);
}
void LoggerModel::ApplyCapabilities(std::string_view protocol, bytes::ByteView capabilities)
{
    for (const auto& p : definition_.parameters)
    {
        if (p.protocol != protocol)
        {
            continue;
        }
        const auto index = static_cast<std::uint16_t>(Decimal(p.ecu_byte_index));
        const auto bit = static_cast<std::uint8_t>(Decimal(p.ecu_bit));
        SetParameterSupported(protocol, p.id,
                              p.ecu_byte_index != "No byte index" && index < capabilities.size() && bit < 8 &&
                                  (capabilities[index] & (1U << bit)) != 0);
    }
    for (const auto& p : definition_.switches)
    {
        const auto index = static_cast<std::uint16_t>(Decimal(p.ecu_byte_index));
        const auto bit = static_cast<std::uint8_t>(Decimal(p.ecu_bit));
        if (p.protocol == protocol && index < capabilities.size())
        {
            SetSwitchSupported(protocol, p.id, bit < 8 && (capabilities[index] & (1U << bit)) != 0);
        }
    }
}
LoggerSelection LoggerModel::DefaultSelection() const
{
    LoggerSelection result;
    if (!definition_.parameters.empty())
    {
        result.protocol = definition_.parameters.front().protocol;
    }
    for (const auto& p : definition_.parameters)
    {
        if (!ParameterSupported(p.protocol, p.id))
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
        if (SwitchSupported(p.protocol, p.id) && result.switch_ids.size() < 20)
        {
            result.switch_ids.push_back(p.id);
        }
    }
    return result;
}
} // namespace fastecu::logging
