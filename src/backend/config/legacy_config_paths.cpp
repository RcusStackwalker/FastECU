#include "src/backend/config/legacy_config_paths.h"

namespace fastecu::config
{

ConfigPaths paths_from_config_values(const fastecu::definitions::ConfigValuesStructure& values)
{
    ConfigPaths paths;
    paths.base_config_directory = values.base_config_directory.toStdString();
    paths.version_config_directory = values.version_config_directory.toStdString();
    paths.calibration_files_directory = values.calibration_files_directory.toStdString();
    paths.config_files_directory = values.config_files_directory.toStdString();
    paths.definition_files_directory = values.definition_files_directory.toStdString();
    paths.kernel_files_directory = values.kernel_files_directory.toStdString();
    paths.datalog_files_directory = values.datalog_files_directory.toStdString();
    paths.syslog_files_directory = values.syslog_files_directory.toStdString();
    paths.config_file = values.config_file.toStdString();
    paths.menu_file = values.menu_file.toStdString();
    paths.protocols_file = values.protocols_file.toStdString();
    paths.logger_file = values.logger_file.toStdString();
    return paths;
}

} // namespace fastecu::config
