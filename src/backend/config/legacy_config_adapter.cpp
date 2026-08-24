#include "src/backend/config/legacy_config_adapter.h"

#include "src/backend/config/car_model_catalog.h"
#include "src/backend/config/legacy_config_paths.h"
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
    {
        field = qs(value);
    }
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
    {
        values->calibration_files.append(qs(f));
    }
    assign_if_present(values->calibration_files_directory, config.calibration_files_directory);
    assign_if_present(values->use_romraider_definitions, config.use_romraider_definitions);
    values->romraider_definition_files.clear();
    for (const std::string& f : config.romraider_definition_files)
    {
        values->romraider_definition_files.append(qs(f));
    }
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
    {
        config.calibration_files.push_back(f.toStdString());
    }
    config.calibration_files_directory = values->calibration_files_directory.toStdString();
    config.use_romraider_definitions = values->use_romraider_definitions.toStdString();
    for (const QString& f : values->romraider_definition_files)
    {
        config.romraider_definition_files.push_back(f.toStdString());
    }
    config.use_ecuflash_definitions = values->use_ecuflash_definitions.toStdString();
    config.ecuflash_definition_files_directory = values->ecuflash_definition_files_directory.toStdString();
    config.romraider_logger_definition_file = values->romraider_logger_definition_file.toStdString();
    config.datalog_files_directory = values->datalog_files_directory.toStdString();
    return config;
}

// Legacy read_protocols_file (file_actions.cpp:1089-1393) parses <protocols>
// into *local-only* QStringLists (file_actions.cpp:1095-1116/1145-1170) --
// never `configValues->flash_protocol_*` -- and uses those purely as a
// lookup table for the <car_models> loop that follows. Every
// `configValues->flash_protocol_*` list is instead populated exclusively by
// the <car_models> loop (file_actions.cpp:1262-1379), one entry per
// <car_model>, with the protocol-derived fields (alias/ecu/mcu/mode/
// checksum/.../description) cross-referenced by matching the car_model's
// <protocol> text against the local protocol lookup table's `name`
// (file_actions.cpp:1340-1369) -- confirmed independently by
// MainWindow/protocol_select.cpp/vehicle_select.cpp, which all iterate
// `flash_protocol_id`/`flash_protocol_make`/`flash_protocol_protocol_name`
// (car_model-only fields) as the shared length for every parallel list, and
// by FileActions::validate_flash_protocols (file_actions.cpp:150), which
// validates every flash_protocol_* list's length against
// `flash_protocol_id.size()`. So this function takes both catalogs and
// builds car_models.size() rows, not protocols.size() + car_models.size()
// rows.
void copy_car_models_into_legacy(const ProtocolCatalog& protocols, const CarModelCatalog& car_models,
                                 fastecu::definitions::ConfigValuesStructure *values)
{
    values->flash_protocol_id.clear();
    values->flash_protocol_alias.clear();
    values->flash_protocol_make.clear();
    values->flash_protocol_model.clear();
    values->flash_protocol_version.clear();
    values->flash_protocol_type.clear();
    values->flash_protocol_kw.clear();
    values->flash_protocol_hp.clear();
    values->flash_protocol_fuel.clear();
    values->flash_protocol_year.clear();
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

    // Legacy's placeholder for a field it never got around to filling in
    // (file_actions.cpp:1273 et al., `.append(" ")` before the cross-
    // reference loop runs) -- a single space, not empty string. Left as-is
    // for an unmatched protocol_name because legacy's replace() is simply
    // never reached for that row's protocol-derived fields.
    const QString kPlaceholder(" ");

    const std::vector<ResolvedCarModel> resolved = resolve_car_models(protocols, car_models);
    for (std::size_t index = 0; index < resolved.size(); ++index)
    {
        const ResolvedCarModel& entry = resolved[index];
        values->flash_protocol_id.append(QString::number(index));
        values->flash_protocol_make.append(qs(entry.make));
        values->flash_protocol_model.append(qs(entry.model));
        values->flash_protocol_version.append(qs(entry.version));
        values->flash_protocol_type.append(qs(entry.type));
        values->flash_protocol_kw.append(qs(entry.kw));
        values->flash_protocol_hp.append(qs(entry.hp));
        values->flash_protocol_fuel.append(qs(entry.fuel));
        values->flash_protocol_year.append(qs(entry.year));
        values->flash_protocol_protocol_name.append(qs(entry.protocol_name));

        if (entry.protocol.has_value())
        {
            const ProtocolEntry& matched = *entry.protocol;
            values->flash_protocol_alias.append(qs(matched.alias));
            values->flash_protocol_ecu.append(qs(matched.ecu));
            values->flash_protocol_mcu.append(qs(matched.mcu));
            values->flash_protocol_mode.append(qs(matched.mode));
            values->flash_protocol_checksum.append(qs(matched.checksum));
            values->flash_protocol_read.append(qs(matched.read));
            values->flash_protocol_test_write.append(qs(matched.test_write));
            values->flash_protocol_write.append(qs(matched.write));
            values->flash_protocol_flash_transport.append(qs(matched.flash_transport));
            values->flash_protocol_log_transport.append(qs(matched.log_transport));
            values->flash_protocol_log_protocol.append(qs(matched.log_protocol));
            values->flash_protocol_ecu_id_ascii.append(qs(matched.ecu_id_ascii));
            values->flash_protocol_ecu_id_addr.append(qs(matched.ecu_id_addr));
            values->flash_protocol_ecu_id_length.append(qs(matched.ecu_id_length));
            values->flash_protocol_cal_id_ascii.append(qs(matched.cal_id_ascii));
            values->flash_protocol_cal_id_addr.append(qs(matched.cal_id_addr));
            values->flash_protocol_cal_id_length.append(qs(matched.cal_id_length));
            values->flash_protocol_kernel.append(qs(matched.kernel));
            values->flash_protocol_kernel_addr.append(qs(matched.kernel_addr));
            values->flash_protocol_description.append(qs(matched.description));
        }
        else
        {
            values->flash_protocol_alias.append(kPlaceholder);
            values->flash_protocol_ecu.append(kPlaceholder);
            values->flash_protocol_mcu.append(kPlaceholder);
            values->flash_protocol_mode.append(kPlaceholder);
            values->flash_protocol_checksum.append(kPlaceholder);
            values->flash_protocol_read.append(kPlaceholder);
            values->flash_protocol_test_write.append(kPlaceholder);
            values->flash_protocol_write.append(kPlaceholder);
            values->flash_protocol_flash_transport.append(kPlaceholder);
            values->flash_protocol_log_transport.append(kPlaceholder);
            values->flash_protocol_log_protocol.append(kPlaceholder);
            values->flash_protocol_ecu_id_ascii.append(kPlaceholder);
            values->flash_protocol_ecu_id_addr.append(kPlaceholder);
            values->flash_protocol_ecu_id_length.append(kPlaceholder);
            values->flash_protocol_cal_id_ascii.append(kPlaceholder);
            values->flash_protocol_cal_id_addr.append(kPlaceholder);
            values->flash_protocol_cal_id_length.append(kPlaceholder);
            values->flash_protocol_kernel.append(kPlaceholder);
            values->flash_protocol_kernel_addr.append(kPlaceholder);
            values->flash_protocol_description.append(kPlaceholder);
        }
    }
}
} // namespace

LegacyConfigAdapter::LegacyConfigAdapter(IFileSystem& file_system, IResourceBundle& resource_bundle,
                                         IFileRepository& file_repository)
    : file_system_(file_system), resource_bundle_(resource_bundle), file_repository_(file_repository)
{
}

fastecu::definitions::ConfigValuesStructure *
LegacyConfigAdapter::set_base_dirs(fastecu::definitions::ConfigValuesStructure *values, const AppRootInfo& root_info)
{
    ConfigPaths paths = resolve_config_paths(root_info, values->software_version.toStdString());
    copy_paths_into_legacy(paths, values);
    return values;
}

fastecu::definitions::ConfigValuesStructure *
LegacyConfigAdapter::check_config_dirs(fastecu::definitions::ConfigValuesStructure *values)
{
    fastecu::NullEventSink events;
    ConfigPaths paths = paths_from_config_values(*values);
    provision_config_directories(paths, file_system_, resource_bundle_, events);
    return values;
}

fastecu::definitions::ConfigValuesStructure *
LegacyConfigAdapter::read_config_file(fastecu::definitions::ConfigValuesStructure *values)
{
    ConfigPaths paths = paths_from_config_values(*values);
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

fastecu::definitions::ConfigValuesStructure *
LegacyConfigAdapter::save_config_file(fastecu::definitions::ConfigValuesStructure *values)
{
    ConfigPaths paths = paths_from_config_values(*values);
    Result<AppConfig> saved = save_app_config(app_config_from_legacy(values), paths, file_repository_);
    if (saved.has_value())
    {
        copy_app_config_into_legacy(*saved, values);
    }
    return values;
}

fastecu::definitions::ConfigValuesStructure *
LegacyConfigAdapter::read_protocols_file(fastecu::definitions::ConfigValuesStructure *values)
{
    ConfigPaths paths = paths_from_config_values(*values);
    Result<ProtocolCatalog> protocols = load_protocol_catalog(paths, file_repository_);
    Result<CarModelCatalog> car_models = load_car_model_catalog(paths, file_repository_);
    if (protocols.has_value() && car_models.has_value())
    {
        copy_car_models_into_legacy(*protocols, *car_models, values);

        bool selection_is_integer = false;
        const int selected = values->flash_protocol_selected_id.toInt(&selection_is_integer);
        if (!selection_is_integer || selected < 0 || selected >= values->flash_protocol_id.size())
        {
            values->flash_protocol_selected_id = "0";
        }
    }

    // Legacy's final step (file_actions.cpp:1385-1389): validate the
    // populated lists and log (not surface as an error) whatever
    // validate_flash_protocols finds. That method lives on FileActions
    // (src/backend/definitions/file_actions.h/.cpp, unrelated to and
    // unchanged by this branch) rather than here: FileActions's Bazel
    // target already depends on //src/backend/config:legacy_config_adapter
    // (see this package's BUILD.bazel legacy_config_adapter comment), so
    // this adapter calling back into FileActions would form the exact hard
    // Bazel cycle that split was built to avoid. FileActions::read_protocols_file
    // (its one-line delegation to this method) runs the validation
    // immediately after this call instead -- see that method's comment.
    return values;
}

} // namespace fastecu::config
