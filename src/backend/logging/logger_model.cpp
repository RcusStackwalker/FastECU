#include "src/backend/logging/logger_model.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>

namespace fastecu::logging
{
namespace
{
LoggerIdentity Identity(std::string_view protocol, std::string_view id)
{
    return {std::string(protocol), std::string(id)};
}
std::optional<unsigned> Decimal(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\n\r\v\f");
    if (first == std::string_view::npos)
    {
        return std::nullopt;
    }
    text = text.substr(first, text.find_last_not_of(" \t\n\r\v\f") - first + 1);
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? std::optional<unsigned>{value}
                                                                               : std::nullopt;
}
EcuSupport Support(const std::map<LoggerIdentity, EcuSupport>& flags, std::string_view protocol, std::string_view id)
{
    const auto it = flags.find(Identity(protocol, id));
    return it == flags.end() ? EcuSupport::kUnknown : it->second;
}
void Update(std::map<LoggerIdentity, EcuSupport>& flags, std::string_view protocol, std::string_view id,
            EcuSupport value)
{
    if (const auto it = flags.find(Identity(protocol, id)); it != flags.end())
    {
        it->second = value;
    }
}
EcuSupport Capability(std::string_view index_text, std::string_view bit_text, bytes::ByteView bytes)
{
    const auto index = Decimal(index_text);
    const auto bit = Decimal(bit_text);
    if (!index.has_value() || !bit.has_value() || *index >= bytes.size() || *bit >= 8)
    {
        return EcuSupport::kUnknown;
    }
    return (bytes[*index] & (1U << *bit)) != 0 ? EcuSupport::kSupported : EcuSupport::kUnsupported;
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

    for (const auto& parameter : definition_.parameters)
    {
        parameter_support_.emplace(Identity(parameter.protocol, parameter.id), EcuSupport::kUnknown);
    }
    for (const auto& item : definition_.switches)
    {
        switch_support_.emplace(Identity(item.protocol, item.id), EcuSupport::kUnknown);
    }
    selection_ = DefaultSelection();
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
EcuSupport LoggerModel::ParameterSupport(std::string_view protocol, std::string_view id) const
{
    return Support(parameter_support_, protocol, id);
}
EcuSupport LoggerModel::SwitchSupport(std::string_view protocol, std::string_view id) const
{
    return Support(switch_support_, protocol, id);
}
void LoggerModel::SetParameterSupport(std::string_view protocol, std::string_view id, EcuSupport state)
{
    Update(parameter_support_, protocol, id, state);
}
void LoggerModel::SetSwitchSupport(std::string_view protocol, std::string_view id, EcuSupport state)
{
    Update(switch_support_, protocol, id, state);
}
void LoggerModel::ResetSupport(std::string_view protocol)
{
    for (auto& [key, state] : parameter_support_)
    {
        if (key.first == protocol)
        {
            state = EcuSupport::kUnknown;
        }
    }
    for (auto& [key, state] : switch_support_)
    {
        if (key.first == protocol)
        {
            state = EcuSupport::kUnknown;
        }
    }
}
void LoggerModel::ResetSupport()
{
    for (auto& entry : parameter_support_)
    {
        entry.second = EcuSupport::kUnknown;
    }
    for (auto& entry : switch_support_)
    {
        entry.second = EcuSupport::kUnknown;
    }
}
bool LoggerModel::ParameterAvailable(std::string_view protocol, std::string_view id) const
{
    const auto *p = Parameter(protocol, id);
    const auto state = ParameterSupport(protocol, id);
    return p != nullptr && (state == EcuSupport::kSupported || (state == EcuSupport::kUnknown && p->enabled));
}
bool LoggerModel::SwitchAvailable(std::string_view protocol, std::string_view id) const
{
    const auto *p = SwitchDefinition(protocol, id);
    const auto state = SwitchSupport(protocol, id);
    return p != nullptr && (state == EcuSupport::kSupported || (state == EcuSupport::kUnknown && p->enabled));
}
void LoggerModel::ApplyCapabilities(std::string_view protocol, bytes::ByteView capabilities)
{
    ResetSupport(protocol);
    for (const auto& p : definition_.parameters)
    {
        if (p.protocol == protocol)
        {
            SetParameterSupport(protocol, p.id, Capability(p.ecu_byte_index, p.ecu_bit, capabilities));
        }
    }
    for (const auto& p : definition_.switches)
    {
        if (p.protocol == protocol)
        {
            SetSwitchSupport(protocol, p.id, Capability(p.ecu_byte_index, p.ecu_bit, capabilities));
        }
    }
}
LoggerSelection LoggerModel::DefaultSelection(std::string_view protocol) const
{
    LoggerSelection result;
    result.protocol = protocol.empty() ? selection_.protocol : std::string(protocol);
    if (result.protocol.empty())
    {
        if (!definition_.parameters.empty())
        {
            result.protocol = definition_.parameters.front().protocol;
        }
        else if (!definition_.switches.empty())
        {
            result.protocol = definition_.switches.front().protocol;
        }
    }
    for (const auto& p : definition_.parameters)
    {
        if (p.protocol != result.protocol || !ParameterAvailable(p.protocol, p.id))
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
        if (p.protocol == result.protocol && SwitchAvailable(p.protocol, p.id) && result.switch_ids.size() < 20)
        {
            result.switch_ids.push_back(p.id);
        }
    }
    return result;
}
} // namespace fastecu::logging
