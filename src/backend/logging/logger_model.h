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
    kUnknown,
    kSupported,
    kUnsupported
};

using LoggerIdentity = std::pair<std::string, std::string>;

class LoggerModel
{
  public:
    bool InstallDefinition(LoggerDefinition definition);
    const LoggerDefinition& Definition() const;
    const LoggerSelection& Selection() const;
    void SetSelection(LoggerSelection selection);
    const LoggerParameter *Parameter(std::string_view protocol, std::string_view id) const;
    const LoggerSwitch *SwitchDefinition(std::string_view protocol, std::string_view id) const;
    EcuSupport ParameterSupport(std::string_view protocol, std::string_view id) const;
    EcuSupport SwitchSupport(std::string_view protocol, std::string_view id) const;
    void SetParameterSupport(std::string_view protocol, std::string_view id, EcuSupport state);
    void SetSwitchSupport(std::string_view protocol, std::string_view id, EcuSupport state);
    void ResetSupport(std::string_view protocol);
    void ResetSupport();
    bool ParameterAvailable(std::string_view protocol, std::string_view id) const;
    bool SwitchAvailable(std::string_view protocol, std::string_view id) const;
    void ApplyCapabilities(std::string_view protocol, bytes::ByteView capabilities);
    LoggerSelection DefaultSelection(std::string_view protocol = {}) const;

  private:
    bool installed_ = false;
    LoggerDefinition definition_;
    LoggerSelection selection_;
    std::map<LoggerIdentity, EcuSupport> parameter_support_;
    std::map<LoggerIdentity, EcuSupport> switch_support_;
};

} // namespace fastecu::logging
