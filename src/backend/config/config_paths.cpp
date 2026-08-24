#include "src/backend/config/config_paths.h"

namespace fastecu::config
{

ConfigPaths resolve_config_paths(std::string_view app_root_path, std::string_view version)
{
    ConfigPaths paths;
    paths.base_config_directory = app_root_path;

    const std::string base = std::string(app_root_path) + "/" + std::string(version);

    paths.version_config_directory = base + "/";
    paths.calibration_files_directory = base + "/calibrations/";
    paths.config_files_directory = base + "/config/";
    paths.definition_files_directory = base + "/definitions/";
    paths.kernel_files_directory = base + "/kernels/";
    paths.datalog_files_directory = base + "/datalogs/";
    paths.syslog_files_directory = base + "/syslogs/";
    paths.config_file = paths.config_files_directory + "fastecu.cfg";
    paths.menu_file = paths.config_files_directory + "menu.cfg";
    paths.protocols_file = paths.config_files_directory + "protocols.cfg";
    paths.logger_file = paths.config_files_directory + "logger.cfg";

    return paths;
}

} // namespace fastecu::config
