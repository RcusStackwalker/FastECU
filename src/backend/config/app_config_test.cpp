#include "src/backend/config/app_config.h"
#include <gtest/gtest.h>
#include <map>

using fastecu::ErrorKind;
using fastecu::IFileRepository;
using fastecu::Result;
using fastecu::Status;
using fastecu::config::AppConfig;
using fastecu::config::ConfigPaths;
using fastecu::config::load_app_config;
using fastecu::config::save_app_config;

namespace
{
class InMemoryFileRepository : public IFileRepository
{
  public:
    Result<std::vector<std::uint8_t>> read(std::string_view handle) override
    {
        auto it = files.find(std::string(handle));
        if (it == files.end())
            return fastecu::fail(ErrorKind::InvalidConfig, "no such handle");
        return it->second;
    }
    Status write(std::string_view handle, std::span<const std::uint8_t> data) override
    {
        files[std::string(handle)].assign(data.begin(), data.end());
        return {};
    }
    std::map<std::string, std::vector<std::uint8_t>> files;
};

ConfigPaths test_paths()
{
    ConfigPaths p;
    p.config_file = "fastecu.cfg";
    return p;
}

// Exact content of resources/shared/config/fastecu.cfg as of this writing.
const char *kShippedDefaultConfig = R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
    <software_settings>
        <setting name="window_size">
            <value width="maximized"/>
            <value height="maximized"/>
        </setting>
        <setting name="toolbar_iconsize">
            <value data="32"/>
        </setting>
        <setting name="serial_port">
            <value data="ttyACM0 - OpenPort 2.0"/>
        </setting>
        <setting name="protocol_id">
            <value data="35"/>
        </setting>
        <setting name="flash_transport">
            <value data="iso15765"/>
        </setting>
        <setting name="log_transport">
            <value data="K-Line"/>
        </setting>
        <setting name="log_protocol">
            <value data="SSM"/>
        </setting>
        <setting name="primary_definition_base">
            <value data="romraider"/>
        </setting>
        <setting name="calibration_files"/>
        <setting name="calibration_files_directory">
            <value data="calibrations/"/>
        </setting>
        <setting name="use_romraider_definitions">
            <value data="disabled"/>
        </setting>
        <setting name="romraider_definition_files">
            <value data=""/>
        </setting>
        <setting name="use_ecuflash_definitions">
            <value data="disabled"/>
        </setting>
        <setting name="ecuflash_definition_files_directory">
            <value data=""/>
        </setting>
        <setting name="logger_definition_file">
            <value data="config/logger_cdbg_example.xml"/>
        </setting>
        <setting name="datalog_files_directory">
            <value data="datalogs"/>
        </setting>
    </software_settings>
</config>
)";
} // namespace

TEST(LoadAppConfig, ParsesEveryShippedDefaultSetting)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    repo.files[paths.config_file] =
        std::vector<std::uint8_t>(kShippedDefaultConfig, kShippedDefaultConfig + strlen(kShippedDefaultConfig));

    auto config = load_app_config(paths, repo);

    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->window_width, "maximized");
    EXPECT_EQ(config->window_height, "maximized");
    EXPECT_EQ(config->toolbar_iconsize, "32");
    EXPECT_EQ(config->serial_port, "ttyACM0 - OpenPort 2.0");
    EXPECT_EQ(config->selected_protocol_id, "35");
    EXPECT_EQ(config->selected_flash_transport, "iso15765");
    EXPECT_EQ(config->selected_log_transport, "K-Line");
    EXPECT_EQ(config->selected_log_protocol, "SSM");
    EXPECT_EQ(config->primary_definition_base, "romraider");
    EXPECT_TRUE(config->calibration_files.empty());
    EXPECT_EQ(config->calibration_files_directory, "calibrations/");
    EXPECT_EQ(config->use_romraider_definitions, "disabled");
    EXPECT_TRUE(config->romraider_definition_files.empty()); // single empty-string entry is dropped
    EXPECT_EQ(config->use_ecuflash_definitions, "disabled");
    EXPECT_EQ(config->ecuflash_definition_files_directory, "");
    EXPECT_EQ(config->romraider_logger_definition_file, "config/logger_cdbg_example.xml");
    EXPECT_EQ(config->datalog_files_directory, "datalogs");
}

TEST(LoadAppConfig, InvalidPrimaryDefinitionBaseValueIsDiscarded)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    std::string xml =
        R"(<?xml version="1.0"?><config name="FastECU" version="x"><software_settings>)"
        R"(<setting name="primary_definition_base"><value data="not-a-real-base"/></setting>)"
        R"(</software_settings></config>)";
    repo.files[paths.config_file] = std::vector<std::uint8_t>(xml.begin(), xml.end());

    auto config = load_app_config(paths, repo);

    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->primary_definition_base, ""); // default-constructed, not overwritten
}

// Matches legacy FileActions::read_config_file's gate (file_actions.cpp:
// 606): a <config> element whose name attribute isn't "FastECU" (or is
// absent) is left unparsed -- config stays at its defaults rather than
// picking up settings from a file that isn't really this app's config.
TEST(LoadAppConfig, ConfigElementWithWrongNameAttributeIsNotParsed)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    std::string xml =
        R"(<?xml version="1.0"?><config name="SomeOtherApp" version="x"><software_settings>)"
        R"(<setting name="serial_port"><value data="COM7"/></setting>)"
        R"(</software_settings></config>)";
    repo.files[paths.config_file] = std::vector<std::uint8_t>(xml.begin(), xml.end());

    auto config = load_app_config(paths, repo);

    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->serial_port, ""); // default-constructed, not "COM7"
}

TEST(LoadAppConfig, MissingFileIsPropagatedAsInvalidConfig)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();

    auto config = load_app_config(paths, repo);

    ASSERT_FALSE(config.has_value());
    EXPECT_EQ(config.error().kind, ErrorKind::InvalidConfig);
}

TEST(SaveAppConfig, NormalizesTrailingSlashesOnThreeDirectoryFields)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    AppConfig config;
    config.calibration_files_directory = "calibrations";
    config.ecuflash_definition_files_directory = "ecuflash";
    config.datalog_files_directory = "datalogs";

    auto saved = save_app_config(config, paths, repo);

    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->calibration_files_directory, "calibrations/");
    EXPECT_EQ(saved->ecuflash_definition_files_directory, "ecuflash/");
    EXPECT_EQ(saved->datalog_files_directory, "datalogs/");
}

TEST(SaveAppConfigThenLoadAppConfig, DatalogDirectoryDoesNotRoundTrip)
{
    // CONFIRMED EXISTING BUG (file_actions.cpp:1076 vs :883): save writes the
    // value under XML tag "logfiles_directory"; load only recognizes
    // "datalog_files_directory". This is preserved, not fixed.
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    AppConfig config;
    config.datalog_files_directory = "custom_datalogs/";

    ASSERT_TRUE(save_app_config(config, paths, repo).has_value());
    auto reloaded = load_app_config(paths, repo);

    ASSERT_TRUE(reloaded.has_value());
    EXPECT_NE(reloaded->datalog_files_directory, "custom_datalogs/");
}

TEST(SaveAppConfigThenLoadAppConfig, EveryOtherFieldRoundTrips)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    AppConfig config;
    config.window_width = "1024";
    config.window_height = "768";
    config.toolbar_iconsize = "24";
    config.serial_port = "COM3";
    config.selected_protocol_id = "12";
    config.selected_flash_transport = "K-Line";
    config.selected_log_transport = "CAN";
    config.selected_log_protocol = "SSM";
    config.primary_definition_base = "ecuflash";
    config.calibration_files = {"a.bin", "b.bin"};
    config.calibration_files_directory = "cal/";
    config.use_romraider_definitions = "enabled";
    config.romraider_definition_files = {"def1.xml"};
    config.use_ecuflash_definitions = "enabled";
    config.ecuflash_definition_files_directory = "ecu/";
    config.romraider_logger_definition_file = "logger.xml";

    ASSERT_TRUE(save_app_config(config, paths, repo).has_value());
    auto reloaded = load_app_config(paths, repo);

    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->window_width, "1024");
    EXPECT_EQ(reloaded->window_height, "768");
    EXPECT_EQ(reloaded->toolbar_iconsize, "24");
    EXPECT_EQ(reloaded->serial_port, "COM3");
    EXPECT_EQ(reloaded->selected_protocol_id, "12");
    EXPECT_EQ(reloaded->selected_flash_transport, "K-Line");
    EXPECT_EQ(reloaded->selected_log_transport, "CAN");
    EXPECT_EQ(reloaded->selected_log_protocol, "SSM");
    EXPECT_EQ(reloaded->primary_definition_base, "ecuflash");
    EXPECT_EQ(reloaded->calibration_files, (std::vector<std::string>{"a.bin", "b.bin"}));
    EXPECT_EQ(reloaded->calibration_files_directory, "cal/");
    EXPECT_EQ(reloaded->use_romraider_definitions, "enabled");
    EXPECT_EQ(reloaded->romraider_definition_files, (std::vector<std::string>{"def1.xml"}));
    EXPECT_EQ(reloaded->use_ecuflash_definitions, "enabled");
    EXPECT_EQ(reloaded->ecuflash_definition_files_directory, "ecu/");
    EXPECT_EQ(reloaded->romraider_logger_definition_file, "logger.xml");
}
