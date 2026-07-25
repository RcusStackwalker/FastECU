#pragma once
#include <string>
#include <vector>
#include "src/backend/config/config_paths.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::config
{

struct AppConfig
{
    std::string window_width;
    std::string window_height;
    std::string toolbar_iconsize;
    std::string serial_port;
    std::string selected_protocol_id;
    std::string selected_flash_transport;
    std::string selected_log_transport;
    std::string selected_log_protocol;
    std::string primary_definition_base;
    std::vector<std::string> calibration_files;
    std::string calibration_files_directory;
    std::string use_romraider_definitions;
    std::vector<std::string> romraider_definition_files;
    std::string use_ecuflash_definitions;
    std::string ecuflash_definition_files_directory;
    std::string romraider_logger_definition_file;
    std::string datalog_files_directory;
};

// Replaces FileActions::read_config_file.
Result<AppConfig> load_app_config(const ConfigPaths& paths, IFileRepository& file_repository);

// Replaces FileActions::save_config_file. Returns the normalized config
// (trailing slash appended to calibration_files_directory/
// ecuflash_definition_files_directory/datalog_files_directory when non-empty
// and not already present) so the caller can keep its in-memory copy in
// sync with what was actually written.
Result<AppConfig> save_app_config(AppConfig config, const ConfigPaths& paths,
                                  IFileRepository& file_repository);

} // namespace fastecu::config
