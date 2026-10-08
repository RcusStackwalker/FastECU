#include "src/backend/logging/logger_model.h"

#include <charconv>
#include <cstdint>
#include <optional>
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
std::optional<unsigned> decimal(std::string_view text)
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
EcuSupport support(const std::map<LoggerIdentity, EcuSupport>& flags, std::string_view protocol, std::string_view id)
{
    const auto it = flags.find(identity(protocol, id));
    return it == flags.end() ? EcuSupport::Unknown : it->second;
}
void update(std::map<LoggerIdentity, EcuSupport>& flags, std::string_view protocol, std::string_view id,
            EcuSupport value)
{
    if (const auto it = flags.find(identity(protocol, id)); it != flags.end())
    {
        it->second = value;
    }
}
EcuSupport capability(std::string_view index_text, std::string_view bit_text, bytes::ByteView bytes)
{
    const auto index = decimal(index_text);
    const auto bit = decimal(bit_text);
    if (!index.has_value() || !bit.has_value() || *index >= bytes.size() || *bit >= 8)
    {
        return EcuSupport::Unknown;
    }
    return (bytes[*index] & (1U << *bit)) != 0 ? EcuSupport::Supported : EcuSupport::Unsupported;
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

    for (const auto& parameter : definition_.parameters)
    {
        parameter_support_.emplace(identity(parameter.protocol, parameter.id), EcuSupport::Unknown);
    }
    for (const auto& item : definition_.switches)
    {
        switch_support_.emplace(identity(item.protocol, item.id), EcuSupport::Unknown);
    }
    selection_ = default_selection();
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
EcuSupport LoggerModel::parameter_support(std::string_view protocol, std::string_view id) const
{
    return support(parameter_support_, protocol, id);
}
EcuSupport LoggerModel::switch_support(std::string_view protocol, std::string_view id) const
{
    return support(switch_support_, protocol, id);
}
void LoggerModel::set_parameter_support(std::string_view protocol, std::string_view id, EcuSupport state)
{
    update(parameter_support_, protocol, id, state);
}
void LoggerModel::set_switch_support(std::string_view protocol, std::string_view id, EcuSupport state)
{
    update(switch_support_, protocol, id, state);
}
void LoggerModel::reset_support(std::string_view protocol)
{
    for (auto& [key, state] : parameter_support_)
    {
        if (key.first == protocol)
        {
            state = EcuSupport::Unknown;
        }
    }
    for (auto& [key, state] : switch_support_)
    {
        if (key.first == protocol)
        {
            state = EcuSupport::Unknown;
        }
    }
}
bool LoggerModel::parameter_available(std::string_view protocol, std::string_view id) const
{
    const auto *p = parameter(protocol, id);
    const auto state = parameter_support(protocol, id);
    return p != nullptr && (state == EcuSupport::Supported || (state == EcuSupport::Unknown && p->enabled));
}
bool LoggerModel::switch_available(std::string_view protocol, std::string_view id) const
{
    const auto *p = switch_definition(protocol, id);
    const auto state = switch_support(protocol, id);
    return p != nullptr && (state == EcuSupport::Supported || (state == EcuSupport::Unknown && p->enabled));
}
void LoggerModel::apply_capabilities(std::string_view protocol, bytes::ByteView capabilities)
{
    reset_support(protocol);
    for (const auto& p : definition_.parameters)
    {
        if (p.protocol == protocol)
        {
            set_parameter_support(protocol, p.id, capability(p.ecu_byte_index, p.ecu_bit, capabilities));
        }
    }
    for (const auto& p : definition_.switches)
    {
        if (p.protocol == protocol)
        {
            set_switch_support(protocol, p.id, capability(p.ecu_byte_index, p.ecu_bit, capabilities));
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
        if (!parameter_available(p.protocol, p.id))
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
        if (switch_available(p.protocol, p.id) && result.switch_ids.size() < 20)
        {
            result.switch_ids.push_back(p.id);
        }
    }
    return result;
}
} // namespace fastecu::logging
