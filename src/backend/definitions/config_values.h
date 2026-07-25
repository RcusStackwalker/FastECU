#ifndef FASTECU_BACKEND_DEFINITIONS_CONFIG_VALUES_H
#define FASTECU_BACKEND_DEFINITIONS_CONFIG_VALUES_H

#include <QDir>
#include <QString>
#include <QStringList>

// Extracted from FileActions (it used to be a nested struct there) so that
// src/backend/config/legacy_config_adapter.h/.cpp can reference this value
// type without pulling in the whole FileActions QWidget. That split matters
// because it breaks a Bazel dependency cycle: legacy_config_adapter needs
// this struct, and file_actions.cpp needs to construct a LegacyConfigAdapter
// -- if legacy_config_adapter depended on the full //src/backend/definitions
// target and //src/backend/definitions depended back on
// //src/backend/config:legacy_config_adapter, that's a cycle Bazel rejects
// outright. FileActions re-exposes this type as `FileActions::ConfigValuesStructure`
// via a `using` alias (see file_actions.h) so no external call site's
// spelling changes. This mirrors the existing kernelmemorymodels.h /
// `models` split in src/backend/definitions/BUILD.bazel, carved out for the
// identical reason relative to //src/backend/flash.
namespace fastecu::definitions
{

struct ConfigValuesStructure
{
    QString software_name = "FastECU";
    QString software_title = "FastECU";
    QString software_version = "0.1.0-beta.5";

    QString serial_port = "ttyUSB0";
    QString baudrate = "4800";
    QString window_size = "default";
    QString window_width = "default";
    QString window_height = "default";
    QString toolbar_iconsize = "32";

#if defined Q_OS_UNIX
    QString app_base_config_directory = QDir::homePath() + "/.config/FastECU/";
#elif defined Q_OS_WIN32
    QString app_base_config_directory = QDir::homePath() + "/AppData/Local/FastECU/";
#endif
    QString base_config_directory = app_base_config_directory;
    QString calibration_files_base_directory = base_config_directory + software_version + "/calibrations/";
    QString config_files_base_directory = base_config_directory + software_version + "/config/";
    QString definition_files_base_directory = base_config_directory + software_version + "/definitions/";
    QString kernel_files_base_directory = base_config_directory + software_version + "/kernels/";
    QString datalog_files_base_directory = base_config_directory + software_version + "/datalogs/";
    QString syslog_files_base_directory = base_config_directory + software_version + "/syslogs/";

    QString version_config_directory;
    QString calibration_files_directory;
    QString config_files_directory;
    QString definition_files_directory;
    QString kernel_files_directory;
    QString datalog_files_directory;
    QString syslog_files_directory;

    QString config_file = "fastecu.cfg";
    QString menu_file = "menu.cfg";
    QString logger_file = "logger.cfg";
    QString protocols_file = "protocols.cfg";

    QStringList calibration_files;
    QStringList romraider_definition_files;
    QString ecuflash_definition_files_directory;
    QString romraider_logger_definition_file;
    QString kernel_files;

    QString use_romraider_definitions = "disabled";
    QString use_ecuflash_definitions = "disabled";
    QString primary_definition_base = "ecuflash";

    QStringList ecuflash_def_cal_id;
    QStringList ecuflash_def_cal_id_addr;
    QStringList ecuflash_def_ecu_id;
    QStringList ecuflash_def_filename;
    QStringList romraider_def_cal_id;
    QStringList romraider_def_cal_id_addr;
    QStringList romraider_def_ecu_id;
    QStringList romraider_def_filename;

    QStringList flash_protocol_id;
    QStringList flash_protocol_alias;
    QStringList flash_protocol_make;
    QStringList flash_protocol_model;
    QStringList flash_protocol_version;
    QStringList flash_protocol_type;
    QStringList flash_protocol_kw;
    QStringList flash_protocol_hp;
    QStringList flash_protocol_fuel;
    QStringList flash_protocol_year;
    QStringList flash_protocol_ecu;
    QStringList flash_protocol_mcu;
    QStringList flash_protocol_mode;
    QStringList flash_protocol_checksum;
    QStringList flash_protocol_read;
    QStringList flash_protocol_test_write;
    QStringList flash_protocol_write;
    QStringList flash_protocol_flash_transport;
    QStringList flash_protocol_log_transport;
    QStringList flash_protocol_log_protocol;
    QStringList flash_protocol_ecu_id_ascii;
    QStringList flash_protocol_ecu_id_addr;
    QStringList flash_protocol_ecu_id_length;
    QStringList flash_protocol_cal_id_ascii;
    QStringList flash_protocol_cal_id_addr;
    QStringList flash_protocol_cal_id_length;
    QStringList flash_protocol_kernel;
    QStringList flash_protocol_kernel_addr;
    QStringList flash_protocol_description;
    QStringList flash_protocol_protocol_name;

    QString flash_protocol_selected_id;
    QString flash_protocol_selected_make;
    QString flash_protocol_selected_model;
    QString flash_protocol_selected_version;
    QString flash_protocol_selected_mcu;
    QString flash_protocol_selected_checksum;
    QString flash_protocol_selected_flash_transport;
    QString flash_protocol_selected_log_transport;
    QString flash_protocol_selected_log_protocol;
    QString flash_protocol_selected_protocol_name;
    QString flash_protocol_selected_description;
};

} // namespace fastecu::definitions

#endif // FASTECU_BACKEND_DEFINITIONS_CONFIG_VALUES_H
