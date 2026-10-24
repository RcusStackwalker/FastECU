#include "src/backend/logging/logging_selection_use_case.h"

#include <utility>

namespace fastecu::logging
{

LoggingSelectionUseCase::LoggingSelectionUseCase(LoggerDefinitionService& definitions, LoggerModel& model)
    : definitions_(definitions), model_(model)
{
}

std::expected<InstallOutcome, InstallFailure> LoggingSelectionUseCase::InstallDefinition(const DefinitionSource& source)
{
    // The service resolves without failing today; the step stays so the
    // caller's notice keeps a home if resolution ever reports an error.
    auto handle = definitions_.ResolveDefinitionHandle(source.configured_handle, source.log_protocol,
                                                       source.config_files_directory);
    if (!handle)
    {
        return std::unexpected(InstallFailure{.step = InstallFailure::Step::kResolve, .error = handle.error()});
    }
    auto definition = definitions_.LoadDefinition(*handle);
    if (!definition)
    {
        return std::unexpected(
            InstallFailure{.step = InstallFailure::Step::kLoad, .handle = *handle, .error = definition.error()});
    }
    model_.InstallDefinition(std::move(*definition));
    model_.SetSelection(model_.DefaultSelection(source.log_protocol));
    return InstallOutcome{.resolved_handle = *handle,
                          .bundled_handle_used = source.configured_handle.empty() && !handle->empty()};
}

Result<RestoreOutcome> LoggingSelectionUseCase::RestorePendingSelection(std::string_view conf_handle,
                                                                        std::string_view ecu_id,
                                                                        std::string_view log_protocol)
{
    const auto stored = definitions_.LoadSelection(conf_handle, ecu_id);
    if (!stored)
    {
        return std::unexpected(stored.error());
    }
    // A successful read clears IDs even if no ECU entry or definition exists.
    model_.SetSelection({.protocol = model_.Selection().protocol});
    if (stored->has_value())
    {
        model_.SetSelection(**stored);
        return RestoreOutcome::kRestored;
    }
    if (model_.Definition().parameters.empty() && model_.Definition().switches.empty())
    {
        return RestoreOutcome::kClearedNoDefinition;
    }
    auto selected = definitions_.LoadOrInitializeSelection(conf_handle, ecu_id, model_.DefaultSelection(log_protocol));
    if (!selected)
    {
        return std::unexpected(selected.error());
    }
    model_.SetSelection(std::move(*selected));
    return RestoreOutcome::kInitialized;
}

Status LoggingSelectionUseCase::SavePendingSelection(std::string_view conf_handle, std::string_view ecu_id,
                                                     LoggerSelection selection)
{
    model_.SetSelection(std::move(selection));
    return definitions_.SaveSelection(conf_handle, ecu_id, model_.Selection());
}

} // namespace fastecu::logging
