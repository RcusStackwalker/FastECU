#pragma once

#include <map>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/logging/logger_definition_model.h"

namespace fastecu::logging
{

// Parameters and switches have separate identity namespaces. The definition is
// installed once; support and operator choices never alter its XML defaults.
enum class EcuSupport
{
    Unknown,
    Supported,
    Unsupported
};

using LoggerIdentity = std::pair<std::string, std::string>;

class LoggerModel
{
  public:
    bool install_definition(LoggerDefinition definition);
    const LoggerDefinition& definition() const;
    const LoggerSelection& selection() const;
    void set_selection(LoggerSelection selection);
    const LoggerParameter *parameter(std::string_view protocol, std::string_view id) const;
    const LoggerSwitch *switch_definition(std::string_view protocol, std::string_view id) const;
    EcuSupport parameter_support(std::string_view protocol, std::string_view id) const;
    EcuSupport switch_support(std::string_view protocol, std::string_view id) const;
    void set_parameter_support(std::string_view protocol, std::string_view id, EcuSupport state);
    void set_switch_support(std::string_view protocol, std::string_view id, EcuSupport state);
    void reset_support(std::string_view protocol);
    bool parameter_available(std::string_view protocol, std::string_view id) const;
    bool switch_available(std::string_view protocol, std::string_view id) const;
    void apply_capabilities(std::string_view protocol, bytes::ByteView capabilities);
    LoggerSelection default_selection() const;

  private:
    bool installed_ = false;
    LoggerDefinition definition_;
    LoggerSelection selection_;
    std::map<LoggerIdentity, EcuSupport> parameter_support_;
    std::map<LoggerIdentity, EcuSupport> switch_support_;
};

} // namespace fastecu::logging
