#include "src/backend/config/app_config.h"

#include <cstring>
#include <format>
#include <sstream>
#include <string_view>

#include <pugixml.hpp>

namespace fastecu::config
{

using namespace std::literals::string_view_literals;

namespace
{

void append_trailing_slash_if_missing(std::string& path)
{
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
    {
        path += '/';
    }
}

} // namespace

Result<AppConfig> load_app_config(const ConfigPaths& paths, IFileRepository& file_repository)
{
    Result<std::vector<std::uint8_t>> bytes = file_repository.read(paths.config_file);
    if (!bytes.has_value())
    {
        return std::unexpected(bytes.error());
    }

    pugi::xml_document doc;
    if (pugi::xml_parse_result parsed = doc.load_buffer(bytes->data(), bytes->size()); !parsed)
    {
        return fail(ErrorKind::InvalidConfig, std::format("config parse error: {}", parsed.description()));
    }

    AppConfig config;
    // Matches legacy FileActions::read_config_file's gate (file_actions.cpp:
    // 606): <software_settings> is only parsed when the root <config>
    // element's name attribute is "FastECU" -- any other (or missing) name
    // leaves the whole block skipped and `config` at its defaults, same as
    // legacy silently doing nothing past that check.
    pugi::xml_node config_node = doc.child("config");
    pugi::xml_node settings = (config_node.attribute("name").as_string() == "FastECU"sv)
                                  ? config_node.child("software_settings")
                                  : pugi::xml_node();
    for (pugi::xml_node setting : settings.children("setting"))
    {
        const std::string name = setting.attribute("name").value();
        if (name == "window_size")
        {
            for (pugi::xml_node value : setting.children("value"))
            {
                if (!std::string{value.attribute("width").value()}.empty())
                {
                    config.window_width = value.attribute("width").value();
                }
                else if (!std::string{value.attribute("height").value()}.empty())
                {
                    config.window_height = value.attribute("height").value();
                }
            }
        }
        else if (name == "toolbar_iconsize")
        {
            config.toolbar_iconsize = setting.child("value").attribute("data").value();
        }
        else if (name == "serial_port")
        {
            config.serial_port = setting.child("value").attribute("data").value();
        }
        else if (name == "protocol_id")
        {
            config.selected_protocol_id = setting.child("value").attribute("data").value();
        }
        else if (name == "flash_transport")
        {
            config.selected_flash_transport = setting.child("value").attribute("data").value();
        }
        else if (name == "log_transport")
        {
            config.selected_log_transport = setting.child("value").attribute("data").value();
        }
        else if (name == "log_protocol")
        {
            config.selected_log_protocol = setting.child("value").attribute("data").value();
        }
        else if (name == "primary_definition_base")
        {
            const std::string value = setting.child("value").attribute("data").value();
            if (value == "romraider" || value == "ecuflash")
            {
                config.primary_definition_base = value;
            }
        }
        else if (name == "calibration_files")
        {
            for (pugi::xml_node value : setting.children("value"))
            {
                config.calibration_files.push_back(value.attribute("data").value());
            }
        }
        else if (name == "calibration_files_directory")
        {
            config.calibration_files_directory = setting.child("value").attribute("data").value();
        }
        else if (name == "romraider_definition_files")
        {
            for (pugi::xml_node value : setting.children("value"))
            {
                const std::string filename = value.attribute("data").value();
                if (!filename.empty())
                {
                    config.romraider_definition_files.push_back(filename);
                }
            }
        }
        else if (name == "use_romraider_definitions")
        {
            config.use_romraider_definitions = setting.child("value").attribute("data").value();
        }
        else if (name == "ecuflash_definition_files_directory")
        {
            config.ecuflash_definition_files_directory = setting.child("value").attribute("data").value();
        }
        else if (name == "use_ecuflash_definitions")
        {
            config.use_ecuflash_definitions = setting.child("value").attribute("data").value();
        }
        else if (name == "logger_definition_file")
        {
            config.romraider_logger_definition_file = setting.child("value").attribute("data").value();
        }
        else if (name == "datalog_files_directory")
        {
            config.datalog_files_directory = setting.child("value").attribute("data").value();
        }
    }

    // Matches legacy FileActions::read_config_file (file_actions.cpp:910),
    // which rewrites the config file on every load by calling
    // save_config_file on the just-parsed struct. The save's result is
    // fire-and-forget (legacy disregards it too): a failure here must not be
    // surfaced as a load failure, and the caller of load_app_config sees the
    // pre-save, unnormalized value, not save_app_config's normalized copy.
    (void)save_app_config(config, paths, file_repository);

    return config;
}

Result<AppConfig> save_app_config(AppConfig config, const ConfigPaths& paths, IFileRepository& file_repository)
{
    append_trailing_slash_if_missing(config.calibration_files_directory);
    append_trailing_slash_if_missing(config.ecuflash_definition_files_directory);
    append_trailing_slash_if_missing(config.datalog_files_directory);

    pugi::xml_document doc;
    pugi::xml_node config_node = doc.append_child("config");
    config_node.append_attribute("name") = "FastECU";
    config_node.append_attribute("version") = "0.1.0-beta.5";
    pugi::xml_node settings = config_node.append_child("software_settings");

    auto add_single = [&](std::string_view name, const std::string& data)
    {
        pugi::xml_node setting = settings.append_child("setting");
        setting.append_attribute("name") = name;
        pugi::xml_node value = setting.append_child("value");
        value.append_attribute("data") = data;
    };
    auto add_list = [&](std::string_view name, const std::vector<std::string>& items)
    {
        pugi::xml_node setting = settings.append_child("setting");
        setting.append_attribute("name") = name;
        for (const std::string& item : items)
        {
            pugi::xml_node value = setting.append_child("value");
            value.append_attribute("data") = item;
        }
    };

    pugi::xml_node window = settings.append_child("setting");
    window.append_attribute("name") = "window_size";
    window.append_child("value").append_attribute("width") = config.window_width;
    window.append_child("value").append_attribute("height") = config.window_height;

    add_single("toolbar_iconsize", config.toolbar_iconsize);
    add_single("serial_port", config.serial_port);
    add_single("protocol_id", config.selected_protocol_id);
    add_single("flash_transport", config.selected_flash_transport);
    add_single("log_transport", config.selected_log_transport);
    add_single("log_protocol", config.selected_log_protocol);
    add_single("primary_definition_base", config.primary_definition_base);
    add_list("calibration_files", config.calibration_files);
    add_single("calibration_files_directory", config.calibration_files_directory);
    add_single("use_romraider_definitions", config.use_romraider_definitions);
    add_list("romraider_definition_files", config.romraider_definition_files);
    add_single("use_ecuflash_definitions", config.use_ecuflash_definitions);
    add_single("ecuflash_definition_files_directory", config.ecuflash_definition_files_directory);
    add_single("logger_definition_file", config.romraider_logger_definition_file);
    // CONFIRMED EXISTING BUG, preserved: tag name "logfiles_directory" does
    // not match load_app_config's "datalog_files_directory" reader, so this
    // value never round-trips through a save/load cycle. See
    // file_actions.cpp:1076 vs :883.
    add_single("logfiles_directory", config.datalog_files_directory);

    std::ostringstream stream;
    doc.save(stream, "    ");
    const std::string serialized = stream.str();
    if (Status write_result = file_repository.write(
            paths.config_file, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(serialized.data()),
                                                             serialized.size()));
        !write_result.has_value())
    {
        return std::unexpected(write_result.error());
    }

    return config;
}

} // namespace fastecu::config
