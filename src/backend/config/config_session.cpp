#include "src/backend/config/config_session.h"

#include <format>
#include <optional>
#include <string>
#include <utility>

#include "src/backend/config/provisioning.h"

namespace fastecu::config
{
namespace
{

void default_if_empty(std::string& field, std::string_view fallback)
{
    if (field.empty())
    {
        field = fallback;
    }
}

// The compiled-in defaults the legacy configuration struct carried. A
// nonempty loaded value wins; AppConfig itself keeps "" as its not-present
// value.
AppConfig with_defaults(AppConfig settings, const ConfigPaths& paths)
{
    default_if_empty(settings.window_width, "default");
    default_if_empty(settings.window_height, "default");
    default_if_empty(settings.toolbar_iconsize, "32");
    default_if_empty(settings.serial_port, "ttyUSB0");
    default_if_empty(settings.primary_definition_base, "ecuflash");
    default_if_empty(settings.use_romraider_definitions, "disabled");
    default_if_empty(settings.use_ecuflash_definitions, "disabled");
    default_if_empty(settings.calibration_files_directory, paths.calibration_files_directory);
    default_if_empty(settings.datalog_files_directory, paths.datalog_files_directory);
    return settings;
}

std::unexpected<Error> failed(const Error& error, std::string_view what, std::string_view path)
{
    return std::unexpected(Error{error.kind, std::format("{} {}: {}", what, path, error.detail)});
}

} // namespace

ConfigSession::ConfigSession(const Catalog& catalog, IFileSystem& file_system, IResourceBundle& resource_bundle,
                             IFileRepository& file_repository, IEventSink& events)
    : catalog_(catalog), file_system_(file_system), resource_bundle_(resource_bundle),
      file_repository_(file_repository), events_(events)
{
}

Status ConfigSession::initialize(std::string_view app_root, std::string_view version)
{
    initialized_ = false;
    provisioned_ = {};
    settings_ = {};

    const ConfigPaths paths = resolve_config_paths(app_root, version);
    if (Status provisioned =
            provision_config_directories(paths, file_system_, resource_bundle_, file_repository_, events_);
        !provisioned.has_value())
    {
        return std::unexpected(Error{provisioned.error().kind,
                                     std::format("Unable to provision configuration: {}", provisioned.error().detail)});
    }

    Result<AppConfig> parsed = parse_app_config(paths, file_repository_);
    if (!parsed.has_value())
    {
        return failed(parsed.error(), "Unable to load settings", paths.config_file);
    }
    AppConfig settings = with_defaults(std::move(*parsed), paths);
    // The legacy loader rewrote the file on every load and ignored the
    // result. Keep the rewrite, but observe it: a failure is nonfatal.
    if (Result<AppConfig> rewritten = save_app_config(settings, paths, file_repository_); rewritten.has_value())
    {
        settings = std::move(*rewritten);
    }
    else
    {
        events_.log(LogLevel::Warning,
                    std::format("Unable to save settings {}: {}", paths.config_file, rewritten.error().detail));
    }

    // An id this catalog does not know -- a retired vehicle, or none saved
    // yet -- selects nothing, and the startup gate asks for a vehicle.
    if (!catalog_.find_vehicle(settings.selected_vehicle_id).has_value())
    {
        settings.selected_vehicle_id.clear();
    }

    provisioned_ = paths;
    settings_ = std::move(settings);
    initialized_ = true;
    return {};
}

bool ConfigSession::initialized() const
{
    return initialized_;
}

Status ConfigSession::save()
{
    if (!initialized_)
    {
        return fail(ErrorKind::Internal, "configuration session is not initialized");
    }
    Result<AppConfig> saved = save_app_config(settings_, provisioned_, file_repository_);
    if (!saved.has_value())
    {
        return failed(saved.error(), "Unable to save settings", provisioned_.config_file);
    }
    settings_ = std::move(*saved);
    return {};
}

AppConfig& ConfigSession::settings()
{
    return settings_;
}

const AppConfig& ConfigSession::settings() const
{
    return settings_;
}

ConfigPaths ConfigSession::provisioned_paths() const
{
    return provisioned_;
}

ConfigPaths ConfigSession::effective_paths() const
{
    ConfigPaths paths = provisioned_;
    if (!settings_.calibration_files_directory.empty())
    {
        paths.calibration_files_directory = settings_.calibration_files_directory;
    }
    if (!settings_.datalog_files_directory.empty())
    {
        paths.datalog_files_directory = settings_.datalog_files_directory;
    }
    return paths;
}

std::span<const VehicleSpec> ConfigSession::vehicles() const
{
    return initialized_ ? catalog_.vehicles() : std::span<const VehicleSpec>{};
}

Result<std::size_t> ConfigSession::selected_row() const
{
    if (!initialized_)
    {
        return fail(ErrorKind::Internal, "configuration session is not initialized");
    }
    const std::optional<std::size_t> row = catalog_.find_vehicle(settings_.selected_vehicle_id);
    if (!row.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "no vehicle is selected");
    }
    return *row;
}

const VehicleSpec *ConfigSession::selected_vehicle() const
{
    const Result<std::size_t> row = selected_row();
    return row.has_value() ? &catalog_.vehicles()[*row] : nullptr;
}

Status ConfigSession::select_row(std::size_t row)
{
    if (!initialized_)
    {
        return fail(ErrorKind::Internal, "configuration session is not initialized");
    }
    const std::span<const VehicleSpec> vehicles = catalog_.vehicles();
    if (row >= vehicles.size())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("vehicle row {} is out of range ({} rows)", row, vehicles.size()));
    }
    settings_.selected_vehicle_id = std::string(vehicles[row].id);
    settings_.selected_log_protocol = std::string(vehicles[row].protocol->log_protocol);
    return {};
}

bool ConfigSession::select_by_protocol_name(std::string_view protocol_name)
{
    if (!initialized_)
    {
        return false;
    }
    const std::optional<std::size_t> row = catalog_.last_vehicle_for_protocol(protocol_name);
    return row.has_value() && select_row(*row).has_value();
}

const VehicleSpec *ConfigSession::vehicle_for_alias(std::string_view flash_method) const
{
    return initialized_ ? catalog_.first_vehicle_for_alias(flash_method) : nullptr;
}

} // namespace fastecu::config
