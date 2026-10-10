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

void DefaultIfEmpty(std::string& field, std::string_view fallback)
{
    if (field.empty())
    {
        field = fallback;
    }
}

// The compiled-in defaults the legacy configuration struct carried. A
// nonempty loaded value wins; AppConfig itself keeps "" as its not-present
// value.
AppConfig WithDefaults(AppConfig settings, const ConfigPaths& paths)
{
    DefaultIfEmpty(settings.window_width, "default");
    DefaultIfEmpty(settings.window_height, "default");
    DefaultIfEmpty(settings.toolbar_iconsize, "32");
    DefaultIfEmpty(settings.serial_port, "ttyUSB0");
    DefaultIfEmpty(settings.primary_definition_base, "ecuflash");
    DefaultIfEmpty(settings.use_romraider_definitions, "disabled");
    DefaultIfEmpty(settings.use_ecuflash_definitions, "disabled");
    DefaultIfEmpty(settings.calibration_files_directory, paths.calibration_files_directory);
    DefaultIfEmpty(settings.datalog_files_directory, paths.datalog_files_directory);
    return settings;
}

std::unexpected<Error> Failed(const Error& error, std::string_view what, std::string_view path)
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

Status ConfigSession::Initialize(std::string_view app_root, std::string_view version)
{
    initialized_ = false;
    provisioned_ = {};
    settings_ = {};

    const ConfigPaths paths = ResolveConfigPaths(app_root, version);
    if (Status provisioned =
            ProvisionConfigDirectories(paths, file_system_, resource_bundle_, file_repository_, events_);
        !provisioned.has_value())
    {
        return std::unexpected(Error{provisioned.error().kind,
                                     std::format("Unable to provision configuration: {}", provisioned.error().detail)});
    }

    Result<AppConfig> parsed = ParseAppConfig(paths, file_repository_);
    if (!parsed.has_value())
    {
        return Failed(parsed.error(), "Unable to load settings", paths.config_file);
    }
    AppConfig settings = WithDefaults(std::move(*parsed), paths);
    // The legacy loader rewrote the file on every load and ignored the
    // result. Keep the rewrite, but observe it: a failure is nonfatal.
    if (Result<AppConfig> rewritten = SaveAppConfig(settings, paths, file_repository_); rewritten.has_value())
    {
        settings = std::move(*rewritten);
    }
    else
    {
        events_.Log(LogLevel::kWarning,
                    std::format("Unable to save settings {}: {}", paths.config_file, rewritten.error().detail));
    }

    // An id this catalog does not know -- a retired vehicle, or none saved
    // yet -- selects nothing, and the startup gate asks for a vehicle.
    if (!catalog_.FindVehicle(settings.selected_vehicle_id).has_value())
    {
        settings.selected_vehicle_id.clear();
    }

    provisioned_ = paths;
    settings_ = std::move(settings);
    initialized_ = true;
    return {};
}

bool ConfigSession::Initialized() const
{
    return initialized_;
}

Status ConfigSession::Save()
{
    if (!initialized_)
    {
        return Fail(ErrorKind::kInternal, "configuration session is not initialized");
    }
    Result<AppConfig> saved = SaveAppConfig(settings_, provisioned_, file_repository_);
    if (!saved.has_value())
    {
        return Failed(saved.error(), "Unable to save settings", provisioned_.config_file);
    }
    settings_ = std::move(*saved);
    return {};
}

AppConfig& ConfigSession::Settings()
{
    return settings_;
}

const AppConfig& ConfigSession::Settings() const
{
    return settings_;
}

ConfigPaths ConfigSession::ProvisionedPaths() const
{
    return provisioned_;
}

ConfigPaths ConfigSession::EffectivePaths() const
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

std::span<const VehicleSpec> ConfigSession::Vehicles() const
{
    return initialized_ ? catalog_.Vehicles() : std::span<const VehicleSpec>{};
}

Result<std::size_t> ConfigSession::SelectedRow() const
{
    if (!initialized_)
    {
        return Fail(ErrorKind::kInternal, "configuration session is not initialized");
    }
    const std::optional<std::size_t> row = catalog_.FindVehicle(settings_.selected_vehicle_id);
    if (!row.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "no vehicle is selected");
    }
    return *row;
}

const VehicleSpec *ConfigSession::SelectedVehicle() const
{
    const Result<std::size_t> row = SelectedRow();
    return row.has_value() ? &catalog_.Vehicles()[*row] : nullptr;
}

Status ConfigSession::SelectRow(std::size_t row)
{
    if (!initialized_)
    {
        return Fail(ErrorKind::kInternal, "configuration session is not initialized");
    }
    const std::span<const VehicleSpec> vehicles = catalog_.Vehicles();
    if (row >= vehicles.size())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("vehicle row {} is out of range ({} rows)", row, vehicles.size()));
    }
    settings_.selected_vehicle_id = std::string(vehicles[row].id);
    settings_.selected_log_protocol = std::string(vehicles[row].protocol->log_protocol);
    return {};
}

bool ConfigSession::SelectByProtocolName(std::string_view protocol_name)
{
    if (!initialized_)
    {
        return false;
    }
    const std::optional<std::size_t> row = catalog_.LastVehicleForProtocol(protocol_name);
    return row.has_value() && SelectRow(*row).has_value();
}

const VehicleSpec *ConfigSession::VehicleForAlias(std::string_view flash_method) const
{
    return initialized_ ? catalog_.FirstVehicleForAlias(flash_method) : nullptr;
}

const ProtocolSpec *ConfigSession::FindProtocol(std::string_view name) const
{
    return initialized_ ? catalog_.FindProtocol(name) : nullptr;
}

} // namespace fastecu::config
