#include "src/backend/config/config_session.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{

using fastecu::Error;
using fastecu::ErrorKind;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::config::AppConfig;
using fastecu::config::ConfigPaths;
using fastecu::config::kMissingProtocolField;
using fastecu::config::protocol_field_or_placeholder;
using fastecu::config::ProtocolEntry;
using fastecu::config::resolve_config_paths;
using fastecu::config::testing::ConfigSessionFixture;
using fastecu::config::testing::kVersion;
using fastecu::config::testing::setting;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::AllOf;
using ::testing::HasSubstr;

bool has_log(const RecordingEventSink& events, LogLevel level, std::string_view text)
{
    return std::ranges::any_of(events.logs, [&](const auto& entry)
                               { return entry.first == level && entry.second.find(text) != std::string::npos; });
}

// --- initialization -------------------------------------------------------

TEST(ConfigSessionInitialize, ProvisionsEveryDirectoryUnderTheRoot)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_TRUE(f.session.initialized());
    EXPECT_TRUE(f.file_system.exists(f.paths.config_files_directory));
    EXPECT_TRUE(f.file_system.exists(f.paths.kernel_files_directory));
    EXPECT_TRUE(f.file_system.exists(f.paths.syslog_files_directory));
}

TEST(ConfigSessionInitialize, CustomRootIsHonored)
{
    ConfigSessionFixture f{"/custom"};
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.provisioned_paths().config_file, "/custom/0.1.0-beta.5/config/fastecu.cfg");
    EXPECT_EQ(f.session.provisioned_paths(), resolve_config_paths("/custom", kVersion));
}

TEST(ConfigSessionInitialize, MigratesThePreviousVersionConfigThroughProvisioning)
{
    ConfigSessionFixture f;
    f.file_system.directory_entries[f.paths.base_config_directory].push_back(
        fastecu::DirEntry{.name = "0.1.0-beta.4", .is_directory = true, .modified_time_epoch_seconds = 100});
    f.file_system.files[f.paths.base_config_directory + "/0.1.0-beta.4/config/fastecu.cfg"] = {7};

    ASSERT_THAT(f.initialize(), IsOk());

    EXPECT_EQ(f.file_system.files[f.paths.config_files_directory + "fastecu.cfg"], (std::vector<std::uint8_t>{7}));
}

TEST(ConfigSessionInitialize, CopiesBundledResourcesThroughProvisioning)
{
    ConfigSessionFixture f;
    f.resource_bundle.bundles["kernels"]["k.bin"] = {1};
    f.file_system.files["kernels/k.bin"] = {1};

    ASSERT_THAT(f.initialize(), IsOk());

    EXPECT_TRUE(f.file_system.exists(f.paths.kernel_files_directory + "k.bin"));
}

TEST(ConfigSessionInitialize, AbsentSettingsTakeCompiledInDefaults)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    const AppConfig& s = f.session.settings();
    EXPECT_EQ(s.window_width, "default");
    EXPECT_EQ(s.window_height, "default");
    EXPECT_EQ(s.toolbar_iconsize, "32");
    EXPECT_EQ(s.serial_port, "ttyUSB0");
    EXPECT_EQ(s.primary_definition_base, "ecuflash");
    EXPECT_EQ(s.use_romraider_definitions, "disabled");
    EXPECT_EQ(s.use_ecuflash_definitions, "disabled");
    EXPECT_EQ(s.calibration_files_directory, f.paths.calibration_files_directory);
    EXPECT_EQ(s.datalog_files_directory, f.paths.datalog_files_directory);
    EXPECT_EQ(s.selected_flash_transport, "");
    EXPECT_EQ(s.selected_log_transport, "");
    EXPECT_EQ(s.selected_log_protocol, "");
    EXPECT_EQ(s.ecuflash_definition_files_directory, "");
    EXPECT_EQ(s.romraider_logger_definition_file, "");
    EXPECT_TRUE(s.calibration_files.empty());
    EXPECT_TRUE(s.romraider_definition_files.empty());
}

TEST(ConfigSessionInitialize, LoadedScalarsOverrideDefaults)
{
    ConfigSessionFixture f;
    f.put_settings(setting("serial_port", "COM7") + setting("toolbar_iconsize", "24") +
                   setting("primary_definition_base", "romraider") + setting("use_ecuflash_definitions", "enabled"));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().serial_port, "COM7");
    EXPECT_EQ(f.session.settings().toolbar_iconsize, "24");
    EXPECT_EQ(f.session.settings().primary_definition_base, "romraider");
    EXPECT_EQ(f.session.settings().use_ecuflash_definitions, "enabled");
}

TEST(ConfigSessionInitialize, LoadedFileListsKeepTheirParserSemantics)
{
    ConfigSessionFixture f;
    f.put_settings(R"(<setting name="calibration_files"><value data="a.bin"/><value data="b.bin"/></setting>)"
                   R"(<setting name="romraider_definition_files"><value data=""/><value data="r.xml"/></setting>)");
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().calibration_files, (std::vector<std::string>{"a.bin", "b.bin"}));
    EXPECT_EQ(f.session.settings().romraider_definition_files, (std::vector<std::string>{"r.xml"}));
}

TEST(ConfigSessionInitialize, NormalizesDirectoriesInMemoryAndOnDisk)
{
    ConfigSessionFixture f;
    f.put_settings(setting("calibration_files_directory", "/cal") +
                   setting("ecuflash_definition_files_directory", "C:\\defs\\") +
                   setting("datalog_files_directory", "/logs"));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().calibration_files_directory, "/cal/");
    EXPECT_EQ(f.session.settings().ecuflash_definition_files_directory, "C:\\defs\\"); // backslash accepted
    EXPECT_EQ(f.session.settings().datalog_files_directory, "/logs/");
    EXPECT_THAT(f.text(f.paths.config_file), HasSubstr(R"(data="/cal/")"));
}

TEST(ConfigSessionInitialize, RewriteFailureAfterLoadIsAWarningNotAFailure)
{
    ConfigSessionFixture f;
    f.put_settings(setting("calibration_files_directory", "/cal"));
    f.file_repository.write_errors[f.paths.config_file] = Error{ErrorKind::Internal, "disk full"};

    ASSERT_THAT(f.initialize(), IsOk());

    EXPECT_TRUE(has_log(f.events, LogLevel::Warning, f.paths.config_file));
    EXPECT_TRUE(has_log(f.events, LogLevel::Warning, "disk full"));
    EXPECT_EQ(f.session.settings().calibration_files_directory, "/cal"); // unnormalized: nothing was written
}

TEST(ConfigSessionInitialize, MissingSettingsFileFailsNamingIt)
{
    ConfigSessionFixture f;
    f.file_repository.files.erase(f.paths.config_file);
    EXPECT_THAT(f.initialize(), IsErrWith(ErrorKind::InvalidConfig, HasSubstr(f.paths.config_file)));
    EXPECT_FALSE(f.session.initialized());
}

TEST(ConfigSessionInitialize, MalformedSettingsFileFailsNamingIt)
{
    ConfigSessionFixture f;
    f.put(f.paths.config_file, "<config");
    EXPECT_THAT(f.initialize(), IsErrWith(ErrorKind::InvalidConfig, HasSubstr(f.paths.config_file)));
}

TEST(ConfigSessionInitialize, MissingProtocolsFileFailsNamingIt)
{
    ConfigSessionFixture f;
    f.file_repository.files.erase(f.paths.protocols_file);
    EXPECT_THAT(f.initialize(), IsErrWith(ErrorKind::InvalidConfig, HasSubstr(f.paths.protocols_file)));
}

TEST(ConfigSessionInitialize, EmptyVehicleCatalogIsRejected)
{
    ConfigSessionFixture f;
    f.put_protocols(R"(<config name="FastECU"><protocols/><car_models/></config>)");
    EXPECT_THAT(f.initialize(), IsErrWith(ErrorKind::InvalidConfig, HasSubstr(f.paths.protocols_file)));
    EXPECT_FALSE(f.session.initialized());
}

TEST(ConfigSessionInitialize, ProvisioningFailureCarriesThePathAndReason)
{
    ConfigSessionFixture f;
    f.file_system.create_directory_error = Error{ErrorKind::Internal, "permission denied"};
    EXPECT_THAT(f.initialize(),
                IsErrWith(ErrorKind::Internal, AllOf(HasSubstr("/root"), HasSubstr("permission denied"))));
}

TEST(ConfigSessionInitialize, FailedInitializationExposesNothing)
{
    ConfigSessionFixture f;
    f.put_protocols(R"(<config name="FastECU"><protocols/><car_models/></config>)");
    ASSERT_FALSE(f.initialize().has_value());

    EXPECT_TRUE(f.session.vehicles().empty());
    EXPECT_FALSE(f.session.selected_row().has_value());
    EXPECT_EQ(f.session.selected_vehicle(), nullptr);
    const std::size_t writes = f.file_repository.write_calls.size();
    EXPECT_FALSE(f.session.save().has_value());
    EXPECT_EQ(f.file_repository.write_calls.size(), writes);
}

TEST(ConfigSessionInitialize, AFailedReinitializationDropsTheEarlierState)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.put_protocols(R"(<config name="FastECU"><protocols/><car_models/></config>)");
    ASSERT_FALSE(f.initialize().has_value());
    EXPECT_FALSE(f.session.initialized());
    EXPECT_TRUE(f.session.vehicles().empty());
}

TEST(ConfigSessionInitialize, AccessBeforeInitializationIsChecked)
{
    ConfigSessionFixture f;
    EXPECT_FALSE(f.session.initialized());
    EXPECT_TRUE(f.session.vehicles().empty());
    EXPECT_THAT(f.session.selected_row(), IsErr(ErrorKind::Internal));
    EXPECT_EQ(f.session.selected_vehicle(), nullptr);
    EXPECT_THAT(f.session.save(), IsErr(ErrorKind::Internal));
}

// --- saved row ------------------------------------------------------------

class InvalidSavedId : public ::testing::TestWithParam<std::string>
{
};

TEST_P(InvalidSavedId, SelectsRowZero)
{
    ConfigSessionFixture f;
    f.put_settings(setting("protocol_id", GetParam()));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().selected_protocol_id, "0");
    ASSERT_THAT(f.session.selected_row(), IsOk());
    EXPECT_EQ(*f.session.selected_row(), 0U);
    EXPECT_EQ(f.session.selected_vehicle()->model, "Impreza");
}

// The fixture has four rows, so "4" is the first out-of-range id.
INSTANTIATE_TEST_SUITE_P(ConfigSessionInitialize, InvalidSavedId,
                         ::testing::Values("", "-1", "abc", "1x", " 1", "+1", "4", "99999999999999999999999"));

TEST(ConfigSessionInitialize, ValidSavedIdIsKept)
{
    ConfigSessionFixture f;
    f.put_settings(setting("protocol_id", "2"));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(*f.session.selected_row(), 2U);
    EXPECT_EQ(f.session.selected_vehicle()->model, "Forester");
}

TEST(ConfigSessionInitialize, RestoringTheSavedRowKeepsSavedTransports)
{
    ConfigSessionFixture f;
    // Row 1's protocol defaults would be K-Line / K-Line / MUT_DMA.
    f.put_settings(setting("protocol_id", "1") + setting("flash_transport", "CAN") + setting("log_transport", "J2534") +
                   setting("log_protocol", "SSM"));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().selected_flash_transport, "CAN");
    EXPECT_EQ(f.session.settings().selected_log_transport, "J2534");
    EXPECT_EQ(f.session.settings().selected_log_protocol, "SSM");
}

// --- vehicle records ------------------------------------------------------

TEST(ConfigSessionVehicles, KeepFileOrder)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    const auto vehicles = f.session.vehicles();
    ASSERT_EQ(vehicles.size(), 4U);
    EXPECT_EQ(vehicles[0].model, "Impreza");
    EXPECT_EQ(vehicles[1].model, "Colt");
    EXPECT_EQ(vehicles[2].model, "Forester");
    EXPECT_EQ(vehicles[3].model, "Skyline");
}

TEST(ConfigSessionVehicles, SharedProtocolRowsResolveToTheSameEntry)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    ASSERT_TRUE(f.session.vehicles()[0].protocol.has_value());
    ASSERT_TRUE(f.session.vehicles()[2].protocol.has_value());
    EXPECT_EQ(f.session.vehicles()[0].protocol->description, "Protocol A");
    EXPECT_EQ(f.session.vehicles()[2].protocol->description, "Protocol A");
}

TEST(ConfigSessionVehicles, UnresolvedReferenceStaysNullopt)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.vehicles()[3].protocol_name, "missing_proto");
    EXPECT_FALSE(f.session.vehicles()[3].protocol.has_value());
}

// --- paths ----------------------------------------------------------------

TEST(ConfigSessionPaths, EffectivePathsStartAsTheProvisionedPaths)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.effective_paths(), f.session.provisioned_paths());
}

TEST(ConfigSessionPaths, EditingConfigurableDirectoriesMovesOnlyThose)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    const ConfigPaths provisioned = f.session.provisioned_paths();

    f.session.settings().calibration_files_directory = "/cal/";
    f.session.settings().datalog_files_directory = "/logs/";

    ConfigPaths expected = provisioned;
    expected.calibration_files_directory = "/cal/";
    expected.datalog_files_directory = "/logs/";
    EXPECT_EQ(f.session.effective_paths(), expected);
    EXPECT_EQ(f.session.provisioned_paths(), provisioned);
}

TEST(ConfigSessionPaths, EmptyConfigurableDirectoryFallsBackToProvisioned)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().calibration_files_directory.clear();
    EXPECT_EQ(f.session.effective_paths().calibration_files_directory, f.paths.calibration_files_directory);
}

TEST(ConfigSessionPaths, DefinitionSearchSettingsAreNotResourceLocations)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().ecuflash_definition_files_directory = "/defs/";
    EXPECT_EQ(f.session.effective_paths().definition_files_directory, f.paths.definition_files_directory);
}

// --- saving ---------------------------------------------------------------

TEST(ConfigSessionSave, NormalizesAndUpdatesTheInMemorySettings)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().calibration_files_directory = "/new";
    f.session.settings().serial_port = "COM3";

    ASSERT_THAT(f.session.save(), IsOk());

    EXPECT_EQ(f.session.settings().calibration_files_directory, "/new/");
    EXPECT_THAT(f.text(f.paths.config_file), HasSubstr(R"(data="COM3")"));
}

TEST(ConfigSessionSave, FailureKeepsEditsAndNamesTheFile)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().calibration_files_directory = "/new";
    f.session.settings().serial_port = "COM3";
    f.file_repository.write_errors[f.paths.config_file] = Error{ErrorKind::InvalidConfig, "cannot open file"};

    EXPECT_THAT(f.session.save(), IsErrWith(ErrorKind::InvalidConfig,
                                            AllOf(HasSubstr(f.paths.config_file), HasSubstr("cannot open file"))));
    EXPECT_EQ(f.session.settings().calibration_files_directory, "/new");
    EXPECT_EQ(f.session.settings().serial_port, "COM3");
}

TEST(ConfigSessionSave, EveryOtherSettingRoundTripsThroughARestart)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().serial_port = "COM3";
    f.session.settings().selected_flash_transport = "CAN";
    f.session.settings().romraider_definition_files = {"r1.xml", "r2.xml"};
    f.session.settings().calibration_files_directory = "/cal/";
    ASSERT_THAT(f.session.save(), IsOk());

    ASSERT_THAT(f.initialize(), IsOk()); // a restart over the same stores
    EXPECT_EQ(f.session.settings().serial_port, "COM3");
    EXPECT_EQ(f.session.settings().selected_flash_transport, "CAN");
    EXPECT_EQ(f.session.settings().romraider_definition_files, (std::vector<std::string>{"r1.xml", "r2.xml"}));
    EXPECT_EQ(f.session.settings().calibration_files_directory, "/cal/");
}

// Known, preserved mismatch: the writer emits logfiles_directory, the reader
// only recognizes datalog_files_directory. Do not fix it here.
TEST(ConfigSessionSave, DatalogDirectoryDoesNotRoundTrip)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().datalog_files_directory = "/elsewhere/";
    ASSERT_THAT(f.session.save(), IsOk());
    EXPECT_THAT(f.text(f.paths.config_file), HasSubstr(R"(name="logfiles_directory")"));

    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().datalog_files_directory, f.paths.datalog_files_directory);
}

// --- selection ------------------------------------------------------------

TEST(ConfigSessionSelect, RowChangesTheSavedRowAndLogProtocolOnly)
{
    ConfigSessionFixture f;
    f.put_settings(setting("flash_transport", "CAN") + setting("log_transport", "J2534") +
                   setting("log_protocol", "SSM"));
    ASSERT_THAT(f.initialize(), IsOk());

    ASSERT_THAT(f.session.select_row(1), IsOk());

    EXPECT_EQ(f.session.settings().selected_protocol_id, "1");
    EXPECT_EQ(f.session.settings().selected_log_protocol, "MUT_DMA");
    EXPECT_EQ(f.session.settings().selected_flash_transport, "CAN");
    EXPECT_EQ(f.session.settings().selected_log_transport, "J2534");
    EXPECT_EQ(f.session.selected_vehicle()->model, "Colt");
}

TEST(ConfigSessionSelect, InvalidRowFailsWithoutChangingSettings)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    const AppConfig before = f.session.settings();

    EXPECT_THAT(f.session.select_row(4), IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(f.session.settings(), before);
}

TEST(ConfigSessionSelect, BeforeInitializationFails)
{
    ConfigSessionFixture f;
    EXPECT_THAT(f.session.select_row(0), IsErr(ErrorKind::Internal));
    EXPECT_FALSE(f.session.select_by_protocol_name("proto_a"));
}

TEST(ConfigSessionSelect, SelectionDoesNotSave)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    const std::size_t writes = f.file_repository.write_calls.size();
    ASSERT_THAT(f.session.select_row(2), IsOk());
    EXPECT_TRUE(f.session.select_by_protocol_name("proto_b"));
    EXPECT_EQ(f.file_repository.write_calls.size(), writes);
}

TEST(ConfigSessionSelect, ProtocolNameUsesTheLastMatchingRow)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());

    EXPECT_TRUE(f.session.select_by_protocol_name("proto_a")); // rows 0 and 2

    EXPECT_EQ(*f.session.selected_row(), 2U);
    EXPECT_EQ(f.session.settings().selected_log_protocol, "SSM");
}

TEST(ConfigSessionSelect, UnmatchedProtocolNameChangesNothing)
{
    ConfigSessionFixture f;
    f.put_settings(setting("protocol_id", "1") + setting("log_protocol", "CDBG"));
    ASSERT_THAT(f.initialize(), IsOk());
    const AppConfig before = f.session.settings();

    EXPECT_FALSE(f.session.select_by_protocol_name("no_such_protocol"));
    EXPECT_EQ(f.session.settings(), before);
}

TEST(ConfigSessionSelect, UnresolvedRowSelectsWithPlaceholders)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());

    EXPECT_TRUE(f.session.select_by_protocol_name("missing_proto"));

    EXPECT_EQ(*f.session.selected_row(), 3U);
    EXPECT_EQ(f.session.settings().selected_log_protocol, std::string(kMissingProtocolField));
    const auto& vehicle = *f.session.selected_vehicle();
    EXPECT_FALSE(vehicle.protocol.has_value());
    EXPECT_EQ(protocol_field_or_placeholder(vehicle, &ProtocolEntry::mcu), " ");
    EXPECT_EQ(protocol_field_or_placeholder(vehicle, &ProtocolEntry::read), " "); // not "yes": unavailable
}

TEST(ConfigSessionSelect, PlaceholderHelperReturnsResolvedFields)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(protocol_field_or_placeholder(f.session.vehicles()[1], &ProtocolEntry::checksum), "n/a");
    EXPECT_EQ(protocol_field_or_placeholder(f.session.vehicles()[0], &ProtocolEntry::description), "Protocol A");
}

} // namespace
