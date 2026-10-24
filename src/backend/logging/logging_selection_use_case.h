#pragma once

#include <expected>
#include <string>

#include "src/backend/logging/logger_definition_service.h"
#include "src/backend/logging/logger_model.h"
#include "src/backend/ports/error.h"

namespace fastecu::logging
{

struct DefinitionSource
{
    std::string configured_handle;      // AppConfig::romraider_logger_definition_file
    std::string log_protocol;           // AppConfig::selected_log_protocol
    std::string config_files_directory; // ConfigPaths::config_files_directory
};

struct InstallOutcome
{
    std::string resolved_handle;
    // True when no handle was configured and resolution produced one; the
    // caller persists resolved_handle into its settings.
    bool bundled_handle_used = false;
};

// Install fails at one of two steps, and the operator text differs per step.
struct InstallFailure
{
    enum class Step
    {
        kResolve,
        kLoad,
    };
    Step step;
    std::string handle; // empty for kResolve
    Error error;
};

// Installs the logging definition into the model. The caller owns the
// operator text, its settings, and the ECU identity.
class LoggingSelectionUseCase
{
  public:
    LoggingSelectionUseCase(LoggerDefinitionService& definitions, LoggerModel& model);

    // On success installs the definition and resets the selection to its
    // default for source.log_protocol. On failure the model is untouched.
    std::expected<InstallOutcome, InstallFailure> InstallDefinition(const DefinitionSource& source);

  private:
    LoggerDefinitionService& definitions_;
    LoggerModel& model_;
};

} // namespace fastecu::logging
