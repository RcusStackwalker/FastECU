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
    bool parameter_supported(std::string_view protocol, std::string_view id) const;
    bool switch_supported(std::string_view protocol, std::string_view id) const;
    void set_parameter_supported(std::string_view protocol, std::string_view id, bool supported);
    void set_switch_supported(std::string_view protocol, std::string_view id, bool supported);
    void apply_capabilities(std::string_view protocol, bytes::ByteView capabilities);
    LoggerSelection default_selection() const;

  private:
    bool installed_ = false;
    LoggerDefinition definition_;
    LoggerSelection selection_;
    std::map<LoggerIdentity, bool> parameter_support_;
    std::map<LoggerIdentity, bool> switch_support_;
};

} // namespace fastecu::logging
