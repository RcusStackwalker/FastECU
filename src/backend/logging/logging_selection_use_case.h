#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "src/backend/logging/logger_definition_service.h"
#include "src/backend/logging/logger_model.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"

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

// Install fails only when the resolved handle cannot be loaded.
struct InstallFailure
{
    std::string handle;
    // As InstallOutcome's: the caller persists `handle` even though loading it
    // failed, as resolution already chose it.
    bool bundled_handle_used = false;
    Error error;
};

enum class RestoreOutcome
{
    kRestored,            // the ECU's pending logging selection was applied
    kInitialized,         // no entry: the definition default was applied and persisted
    kClearedNoDefinition, // no entry and an empty definition: IDs cleared, nothing persisted
};

// Installs the logging definition and restores or saves the pending logging
// selection in the model. The caller owns the operator text, its settings,
// and the ECU identity.
class LoggingSelectionUseCase
{
  public:
    LoggingSelectionUseCase(LoggerDefinitionService& definitions, LoggerModel& model);

    // On success installs the definition and resets the selection to its
    // default for source.log_protocol. On failure the model is untouched.
    // The model installs one definition only: if it already holds one, the
    // install is refused and ignored, yet this still reports success and
    // resets the selection. Production installs once at startup; a reload
    // would need this to surface the refusal.
    std::expected<InstallOutcome, InstallFailure> InstallDefinition(const DefinitionSource& source);

    // An error means a conf read, or the initializing write, failed. A failed
    // first read keeps the selection; any later failure leaves its IDs cleared.
    Result<RestoreOutcome> RestorePendingSelection(std::string_view conf_handle, std::string_view ecu_id,
                                                   std::string_view log_protocol);

    // Applies `selection` to the model, then persists it for `ecu_id`. The
    // model keeps the selection even when the save fails.
    Status SavePendingSelection(std::string_view conf_handle, std::string_view ecu_id, LoggerSelection selection);

  private:
    LoggerDefinitionService& definitions_;
    LoggerModel& model_;
};

} // namespace fastecu::logging
