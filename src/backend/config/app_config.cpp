#include "src/backend/config/app_config.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <sstream>
#include <string_view>
#include <string>
#include <tuple>
#include <vector>

#include <pugixml.hpp>

namespace fastecu::config
{

using namespace std::literals::string_view_literals;

namespace
{

void AppendTrailingSlashIfMissing(std::string& path)
{
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
    {
        path += '/';
    }
}

bool IsDefinitionBase(std::string_view value)
{
    return value == "romraider" || value == "ecuflash";
}

bool IsNonEmpty(std::string_view value)
{
    return !value.empty();
}

// A setting stored as one value's data attribute, or as a list of them.
struct SettingField
{
    std::string_view name;
    std::string AppConfig::*single = nullptr;
    std::vector<std::string> AppConfig::*list = nullptr;
    // Values it rejects are ignored on load. Null accepts every value.
    bool (*accepts)(std::string_view) = nullptr;
};

// window_size is saved first, then these in order.
constexpr auto kSettingFields = std::to_array<SettingField>({
    {.name = "toolbar_iconsize", .single = &AppConfig::toolbar_iconsize},
    {.name = "serial_port", .single = &AppConfig::serial_port},
    {.name = "vehicle_id", .single = &AppConfig::selected_vehicle_id},
    {.name = "flash_transport", .single = &AppConfig::selected_flash_transport},
    {.name = "log_transport", .single = &AppConfig::selected_log_transport},
    {.name = "log_protocol", .single = &AppConfig::selected_log_protocol},
    {.name = "primary_definition_base", .single = &AppConfig::primary_definition_base, .accepts = IsDefinitionBase},
    {.name = "calibration_files", .list = &AppConfig::calibration_files},
    {.name = "calibration_files_directory", .single = &AppConfig::calibration_files_directory},
    {.name = "use_romraider_definitions", .single = &AppConfig::use_romraider_definitions},
    {.name = "romraider_definition_files", .list = &AppConfig::romraider_definition_files, .accepts = IsNonEmpty},
    {.name = "use_ecuflash_definitions", .single = &AppConfig::use_ecuflash_definitions},
    {.name = "ecuflash_definition_files_directory", .single = &AppConfig::ecuflash_definition_files_directory},
    {.name = "logger_definition_file", .single = &AppConfig::romraider_logger_definition_file},
    {.name = "datalog_files_directory", .single = &AppConfig::datalog_files_directory},
});

void ParseWindowSize(pugi::xml_node setting, AppConfig& config)
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

void ParseField(pugi::xml_node setting, const SettingField& field, AppConfig& config)
{
    const auto accepted = [&field](std::string_view value) { return field.accepts == nullptr || field.accepts(value); };
    if (field.single != nullptr)
    {
        if (const std::string value = setting.child("value").attribute("data").value(); accepted(value))
        {
            config.*field.single = value;
        }
        return;
    }
    for (pugi::xml_node value : setting.children("value"))
    {
        if (const std::string data = value.attribute("data").value(); accepted(data))
        {
            (config.*field.list).push_back(data);
        }
    }
}

void ParseSetting(pugi::xml_node setting, AppConfig& config)
{
    const std::string_view name = setting.attribute("name").value();
    if (name == "window_size")
    {
        ParseWindowSize(setting, config);
        return;
    }
    if (const auto field = std::ranges::find(kSettingFields, name, &SettingField::name); field != kSettingFields.end())
    {
        ParseField(setting, *field, config);
    }
}

void SaveField(pugi::xml_node settings, const SettingField& field, const AppConfig& config)
{
    pugi::xml_node setting = settings.append_child("setting");
    setting.append_attribute("name") = field.name;
    const std::vector<std::string> values =
        field.single != nullptr ? std::vector{config.*field.single} : config.*field.list;
    for (const std::string& value : values)
    {
        setting.append_child("value").append_attribute("data") = value;
    }
}

} // namespace

Result<AppConfig> ParseAppConfig(const ConfigPaths& paths, IFileRepository& file_repository)
{
    Result<std::vector<std::uint8_t>> bytes = file_repository.Read(paths.config_file);
    if (!bytes.has_value())
    {
        return std::unexpected(bytes.error());
    }

    pugi::xml_document doc;
    if (pugi::xml_parse_result parsed = doc.load_buffer(bytes->data(), bytes->size()); !parsed)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("config parse error: {}", parsed.description()));
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
        ParseSetting(setting, config);
    }
    return config;
}

Result<AppConfig> LoadAppConfig(const ConfigPaths& paths, IFileRepository& file_repository)
{
    Result<AppConfig> config = ParseAppConfig(paths, file_repository);
    if (!config.has_value())
    {
        return config;
    }
    // Matches legacy FileActions::read_config_file (file_actions.cpp:910),
    // which rewrites the config file on every load. The save's result is
    // fire-and-forget; callers needing to observe it use parse_app_config
    // and save_app_config themselves (ConfigSession does).
    std::ignore = SaveAppConfig(*config, paths, file_repository);
    return config;
}

Result<AppConfig> SaveAppConfig(AppConfig config, const ConfigPaths& paths, IFileRepository& file_repository)
{
    AppendTrailingSlashIfMissing(config.calibration_files_directory);
    AppendTrailingSlashIfMissing(config.ecuflash_definition_files_directory);
    AppendTrailingSlashIfMissing(config.datalog_files_directory);

    pugi::xml_document doc;
    pugi::xml_node config_node = doc.append_child("config");
    config_node.append_attribute("name") = "FastECU";
    config_node.append_attribute("version") = "0.1.0-beta.5";
    pugi::xml_node settings = config_node.append_child("software_settings");

    pugi::xml_node window = settings.append_child("setting");
    window.append_attribute("name") = "window_size";
    window.append_child("value").append_attribute("width") = config.window_width;
    window.append_child("value").append_attribute("height") = config.window_height;
    for (const SettingField& field : kSettingFields)
    {
        SaveField(settings, field, config);
    }

    std::ostringstream stream;
    doc.save(stream, "    ");
    const std::string serialized = stream.str();
    if (Status write_result = file_repository.Write(
            paths.config_file, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(serialized.data()),
                                                             serialized.size()));
        !write_result.has_value())
    {
        return std::unexpected(write_result.error());
    }

    return config;
}

} // namespace fastecu::config
