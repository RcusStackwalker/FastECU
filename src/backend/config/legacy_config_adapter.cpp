#include "src/backend/config/legacy_config_adapter.h"

#include "src/backend/config/provisioning.h"
#include "src/backend/ports/event_sink.h"

namespace fastecu::config
{
namespace
{
QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

void copy_paths_into_legacy(const ConfigPaths& paths, fastecu::definitions::ConfigValuesStructure *values)
{
    values->base_config_directory = qs(paths.base_config_directory);
    values->version_config_directory = qs(paths.version_config_directory);
    values->calibration_files_directory = qs(paths.calibration_files_directory);
    values->config_files_directory = qs(paths.config_files_directory);
    values->definition_files_directory = qs(paths.definition_files_directory);
    values->kernel_files_directory = qs(paths.kernel_files_directory);
    values->datalog_files_directory = qs(paths.datalog_files_directory);
    values->syslog_files_directory = qs(paths.syslog_files_directory);
    values->config_file = qs(paths.config_file);
    values->menu_file = qs(paths.menu_file);
    values->protocols_file = qs(paths.protocols_file);
    values->logger_file = qs(paths.logger_file);
}

// Reverse of copy_paths_into_legacy: reconstructs a ConfigPaths from whatever
// is currently on `values`. Each of check_config_dirs/read_config_file/
// save_config_file/read_protocols_file calls this itself rather than relying
// on a ConfigPaths cached from a previous set_base_dirs call -- FileActions's
// own methods take configValues fresh on every call with no adapter-side
// state threading them together, and callers (e.g. tests, or code invoking
// read_config_file straight off a hand-populated struct) may reasonably not
// have called set_base_dirs first.
ConfigPaths paths_from_legacy(const fastecu::definitions::ConfigValuesStructure *values)
{
    ConfigPaths paths;
    paths.base_config_directory = values->base_config_directory.toStdString();
    paths.version_config_directory = values->version_config_directory.toStdString();
    paths.calibration_files_directory = values->calibration_files_directory.toStdString();
    paths.config_files_directory = values->config_files_directory.toStdString();
    paths.definition_files_directory = values->definition_files_directory.toStdString();
    paths.kernel_files_directory = values->kernel_files_directory.toStdString();
    paths.datalog_files_directory = values->datalog_files_directory.toStdString();
    paths.syslog_files_directory = values->syslog_files_directory.toStdString();
    paths.config_file = values->config_file.toStdString();
    paths.menu_file = values->menu_file.toStdString();
    paths.protocols_file = values->protocols_file.toStdString();
    paths.logger_file = values->logger_file.toStdString();
    return paths;
}

// AppConfig's fields are blank ("") when load_app_config found no matching
// <setting> element in the file (see app_config_test.cpp's
// InvalidPrimaryDefinitionBaseValueIsDiscarded, which pins "" as AppConfig's
// own not-present sentinel -- it must NOT gain a legacy-matching default of
// its own). The legacy Qt reader only ever assigned `configValues->field`
// when it found the corresponding element (file_actions.cpp's old
// read_config_file, one `if (setting name == ...)` branch per field), so a
// setting genuinely absent from the file left the caller's pre-existing
// value (typically ConfigValuesStructure's compiled-in default, e.g.
// "disabled"/"ecuflash") untouched rather than being blanked out. Skipping
// the copy when AppConfig's value is still "" reproduces that byte-for-byte.
void assign_if_present(QString& field, const std::string& value)
{
    if (!value.empty())
        field = qs(value);
}

void copy_app_config_into_legacy(const AppConfig& config, fastecu::definitions::ConfigValuesStructure *values)
{
    assign_if_present(values->window_width, config.window_width);
    assign_if_present(values->window_height, config.window_height);
    assign_if_present(values->toolbar_iconsize, config.toolbar_iconsize);
    assign_if_present(values->serial_port, config.serial_port);
    assign_if_present(values->flash_protocol_selected_id, config.selected_protocol_id);
    assign_if_present(values->flash_protocol_selected_flash_transport, config.selected_flash_transport);
    assign_if_present(values->flash_protocol_selected_log_transport, config.selected_log_transport);
    assign_if_present(values->flash_protocol_selected_log_protocol, config.selected_log_protocol);
    assign_if_present(values->primary_definition_base, config.primary_definition_base);
    values->calibration_files.clear();
    for (const std::string& f : config.calibration_files)
        values->calibration_files.append(qs(f));
    assign_if_present(values->calibration_files_directory, config.calibration_files_directory);
    assign_if_present(values->use_romraider_definitions, config.use_romraider_definitions);
    values->romraider_definition_files.clear();
    for (const std::string& f : config.romraider_definition_files)
        values->romraider_definition_files.append(qs(f));
    assign_if_present(values->use_ecuflash_definitions, config.use_ecuflash_definitions);
    assign_if_present(values->ecuflash_definition_files_directory, config.ecuflash_definition_files_directory);
    assign_if_present(values->romraider_logger_definition_file, config.romraider_logger_definition_file);
    assign_if_present(values->datalog_files_directory, config.datalog_files_directory);
}

AppConfig app_config_from_legacy(const fastecu::definitions::ConfigValuesStructure *values)
{
    AppConfig config;
    config.window_width = values->window_width.toStdString();
    config.window_height = values->window_height.toStdString();
    config.toolbar_iconsize = values->toolbar_iconsize.toStdString();
    config.serial_port = values->serial_port.toStdString();
    config.selected_protocol_id = values->flash_protocol_selected_id.toStdString();
    config.selected_flash_transport = values->flash_protocol_selected_flash_transport.toStdString();
    config.selected_log_transport = values->flash_protocol_selected_log_transport.toStdString();
    config.selected_log_protocol = values->flash_protocol_selected_log_protocol.toStdString();
    config.primary_definition_base = values->primary_definition_base.toStdString();
    for (const QString& f : values->calibration_files)
        config.calibration_files.push_back(f.toStdString());
    config.calibration_files_directory = values->calibration_files_directory.toStdString();
    config.use_romraider_definitions = values->use_romraider_definitions.toStdString();
    for (const QString& f : values->romraider_definition_files)
        config.romraider_definition_files.push_back(f.toStdString());
    config.use_ecuflash_definitions = values->use_ecuflash_definitions.toStdString();
    config.ecuflash_definition_files_directory = values->ecuflash_definition_files_directory.toStdString();
    config.romraider_logger_definition_file = values->romraider_logger_definition_file.toStdString();
    config.datalog_files_directory = values->datalog_files_directory.toStdString();
    return config;
}

void copy_protocol_catalog_into_legacy(const ProtocolCatalog& catalog,
                                       fastecu::definitions::ConfigValuesStructure *values)
{
    values->flash_protocol_alias.clear();
    values->flash_protocol_ecu.clear();
    values->flash_protocol_mcu.clear();
    values->flash_protocol_mode.clear();
    values->flash_protocol_checksum.clear();
    values->flash_protocol_read.clear();
    values->flash_protocol_test_write.clear();
    values->flash_protocol_write.clear();
    values->flash_protocol_flash_transport.clear();
    values->flash_protocol_log_transport.clear();
    values->flash_protocol_log_protocol.clear();
    values->flash_protocol_ecu_id_ascii.clear();
    values->flash_protocol_ecu_id_addr.clear();
    values->flash_protocol_ecu_id_length.clear();
    values->flash_protocol_cal_id_ascii.clear();
    values->flash_protocol_cal_id_addr.clear();
    values->flash_protocol_cal_id_length.clear();
    values->flash_protocol_kernel.clear();
    values->flash_protocol_kernel_addr.clear();
    values->flash_protocol_description.clear();
    values->flash_protocol_protocol_name.clear();

    for (const ProtocolEntry& entry : catalog)
    {
        values->flash_protocol_alias.append(qs(entry.alias));
        values->flash_protocol_ecu.append(qs(entry.ecu));
        values->flash_protocol_mcu.append(qs(entry.mcu));
        values->flash_protocol_mode.append(qs(entry.mode));
        values->flash_protocol_checksum.append(entry.checksum ? "yes" : "no");
        values->flash_protocol_read.append(entry.read ? "yes" : "no");
        values->flash_protocol_test_write.append(entry.test_write ? "yes" : "no");
        values->flash_protocol_write.append(entry.write ? "yes" : "no");
        values->flash_protocol_flash_transport.append(qs(entry.flash_transport));
        values->flash_protocol_log_transport.append(qs(entry.log_transport));
        values->flash_protocol_log_protocol.append(qs(entry.log_protocol));
        values->flash_protocol_ecu_id_ascii.append(entry.ecu_id_ascii ? "yes" : "no");
        values->flash_protocol_ecu_id_addr.append(qs(entry.ecu_id_addr));
        values->flash_protocol_ecu_id_length.append(qs(entry.ecu_id_length));
        values->flash_protocol_cal_id_ascii.append(entry.cal_id_ascii ? "yes" : "no");
        values->flash_protocol_cal_id_addr.append(qs(entry.cal_id_addr));
        values->flash_protocol_cal_id_length.append(qs(entry.cal_id_length));
        values->flash_protocol_kernel.append(qs(entry.kernel));
        values->flash_protocol_kernel_addr.append(qs(entry.kernel_addr));
        values->flash_protocol_description.append(qs(entry.description));
        values->flash_protocol_protocol_name.append(qs(entry.protocol_name));
    }
}
} // namespace

LegacyConfigAdapter::LegacyConfigAdapter(IFileSystem& file_system, IResourceBundle& resource_bundle,
                                         IFileRepository& file_repository)
    : file_system_(file_system), resource_bundle_(resource_bundle), file_repository_(file_repository)
{
}

fastecu::definitions::ConfigValuesStructure *LegacyConfigAdapter::set_base_dirs(
    fastecu::definitions::ConfigValuesStructure *values, const AppRootInfo& root_info)
{
    ConfigPaths paths = resolve_config_paths(root_info, values->software_version.toStdString());
    copy_paths_into_legacy(paths, values);
    return values;
}

fastecu::definitions::ConfigValuesStructure *LegacyConfigAdapter::check_config_dirs(
    fastecu::definitions::ConfigValuesStructure *values)
{
    fastecu::NullEventSink events;
    ConfigPaths paths = paths_from_legacy(values);
    provision_config_directories(paths, file_system_, resource_bundle_, events);
    return values;
}

fastecu::definitions::ConfigValuesStructure *LegacyConfigAdapter::read_config_file(
    fastecu::definitions::ConfigValuesStructure *values)
{
    ConfigPaths paths = paths_from_legacy(values);
    Result<AppConfig> config = load_app_config(paths, file_repository_);
    if (config.has_value())
    {
        // Legacy read_config_file called save_config_file(configValues) on
        // the same shared struct as its last step, so save's trailing-slash
        // normalization (calibration_files_directory/
        // ecuflash_definition_files_directory/datalog_files_directory)
        // landed in the in-memory struct immediately -- callers saw
        // normalized paths right after read_config_file returned, not only
        // on a subsequent read. load_app_config deliberately returns the
        // pre-save parsed value (see app_config.cpp's own comment and
        // app_config_test.cpp's contract for it), so reproduce the second
        // step explicitly here: save again (idempotent; matches legacy's
        // actual outcome) and copy the *normalized* result, not the raw
        // parse. A save failure here must not be surfaced as a load
        // failure (same reasoning as app_config.cpp's own fire-and-forget
        // save), so fall back to the unnormalized parse if it fails.
        Result<AppConfig> saved = save_app_config(*config, paths, file_repository_);
        copy_app_config_into_legacy(saved.has_value() ? *saved : *config, values);
    }
    return values;
}

fastecu::definitions::ConfigValuesStructure *LegacyConfigAdapter::save_config_file(
    fastecu::definitions::ConfigValuesStructure *values)
{
    ConfigPaths paths = paths_from_legacy(values);
    Result<AppConfig> saved =
        save_app_config(app_config_from_legacy(values), paths, file_repository_);
    if (saved.has_value())
        copy_app_config_into_legacy(*saved, values);
    return values;
}

fastecu::definitions::ConfigValuesStructure *LegacyConfigAdapter::read_protocols_file(
    fastecu::definitions::ConfigValuesStructure *values)
{
    ConfigPaths paths = paths_from_legacy(values);
    Result<ProtocolCatalog> catalog = load_protocol_catalog(paths, file_repository_);
    if (catalog.has_value())
        copy_protocol_catalog_into_legacy(*catalog, values);
    return values;
}

} // namespace fastecu::config
