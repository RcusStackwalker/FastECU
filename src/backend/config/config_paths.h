#pragma once
#include <string>
#include <string_view>

namespace fastecu::config
{

struct ConfigPaths
{
    bool operator==(const ConfigPaths&) const = default;

    std::string base_config_directory;
    std::string version_config_directory;
    std::string calibration_files_directory;
    std::string config_files_directory;
    std::string definition_files_directory;
    std::string kernel_files_directory;
    std::string datalog_files_directory;
    std::string syslog_files_directory;
    std::string config_file;
    std::string logger_file;
};

// Pure, no I/O. Replaces FileActions::set_base_dirs.
ConfigPaths ResolveConfigPaths(std::string_view app_root_path, std::string_view version);

} // namespace fastecu::config
