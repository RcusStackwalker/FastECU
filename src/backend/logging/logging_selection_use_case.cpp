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

} // namespace fastecu::logging
