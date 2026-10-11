#include "src/backend/config/config_session.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/config/builtin_catalog.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{

using fastecu::Error;
using fastecu::ErrorKind;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::config::AppConfig;
using fastecu::config::BuiltinCatalog;
using fastecu::config::ConfigPaths;
using fastecu::config::ResolveConfigPaths;
using fastecu::config::testing::ConfigSessionFixture;
using fastecu::config::testing::kVersion;
using fastecu::config::testing::Setting;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::AllOf;
using ::testing::HasSubstr;

template <typename CatalogReference>
constexpr bool kCanConstructSession =
    std::is_constructible_v<fastecu::config::ConfigSession, CatalogReference, fastecu::IFileSystem&,
                            fastecu::IResourceBundle&, fastecu::IFileRepository&, fastecu::IEventSink&>;

static_assert(kCanConstructSession<fastecu::config::Catalog&>);
static_assert(kCanConstructSession<const fastecu::config::Catalog&>);
static_assert(!kCanConstructSession<fastecu::config::Catalog&&>);

bool HasLog(const RecordingEventSink& events, LogLevel level, std::string_view text)
{
    return std::ranges::any_of(events.logs, [&](const auto& entry)
                               { return entry.first == level && entry.second.find(text) != std::string::npos; });
}

// --- initialization -------------------------------------------------------

TEST(ConfigSessionInitialize, ProvisionsEveryDirectoryUnderTheRoot)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_TRUE(f.session.Initialized());
    EXPECT_TRUE(f.file_system.Exists(f.paths.config_files_directory));
    EXPECT_TRUE(f.file_system.Exists(f.paths.kernel_files_directory));
    EXPECT_TRUE(f.file_system.Exists(f.paths.syslog_files_directory));
}

TEST(ConfigSessionInitialize, CustomRootIsHonored)
{
    ConfigSessionFixture f{"/custom"};
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.ProvisionedPaths().config_file, "/custom/0.1.0-beta.5/config/fastecu.cfg");
    EXPECT_EQ(f.session.ProvisionedPaths(), ResolveConfigPaths("/custom", kVersion));
}

TEST(ConfigSessionInitialize, CopiesBundledResourcesThroughProvisioning)
{
    ConfigSessionFixture f;
    f.resource_bundle.bundles["kernels"]["k.bin"] = {1};

    ASSERT_THAT(f.Initialize(), IsOk());

    EXPECT_EQ(f.file_repository.files.at(f.paths.kernel_files_directory + "k.bin"), (std::vector<std::uint8_t>{1}));
}

TEST(ConfigSessionInitialize, AbsentSettingsTakeCompiledInDefaults)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    const AppConfig& s = f.session.Settings();
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
    f.PutSettings(R"(<setting name="window_size"><value width="1024"/><value height="768"/></setting>)" +
                  Setting("serial_port", "COM7") + Setting("toolbar_iconsize", "24") +
                  Setting("vehicle_id", "subaru-forester-v3") + Setting("flash_transport", "iso15765") +
                  Setting("log_transport", "K-Line") + Setting("log_protocol", "SSM") +
                  Setting("primary_definition_base", "romraider") + Setting("use_ecuflash_definitions", "enabled"));
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.Settings().window_width, "1024");
    EXPECT_EQ(f.session.Settings().window_height, "768");
    EXPECT_EQ(f.session.Settings().serial_port, "COM7");
    EXPECT_EQ(f.session.Settings().toolbar_iconsize, "24");
    EXPECT_EQ(f.session.Settings().selected_vehicle_id, "subaru-forester-v3");
    EXPECT_EQ(f.session.Settings().selected_flash_transport, "iso15765");
    EXPECT_EQ(f.session.Settings().selected_log_transport, "K-Line");
    EXPECT_EQ(f.session.Settings().selected_log_protocol, "SSM");
    EXPECT_EQ(f.session.Settings().primary_definition_base, "romraider");
    EXPECT_EQ(f.session.Settings().use_ecuflash_definitions, "enabled");
}

TEST(ConfigSessionInitialize, LoadedFileListsKeepTheirParserSemantics)
{
    ConfigSessionFixture f;
    f.PutSettings(R"(<setting name="calibration_files"><value data="a.bin"/><value data="b.bin"/></setting>)"
                  R"(<setting name="romraider_definition_files"><value data=""/><value data="r.xml"/></setting>)");
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.Settings().calibration_files, (std::vector<std::string>{"a.bin", "b.bin"}));
    EXPECT_EQ(f.session.Settings().romraider_definition_files, (std::vector<std::string>{"r.xml"}));
}

TEST(ConfigSessionInitialize, NormalizesDirectoriesInMemoryAndOnDisk)
{
    ConfigSessionFixture f;
    f.PutSettings(Setting("calibration_files_directory", "/cal") +
                  Setting("ecuflash_definition_files_directory", "C:\\defs\\") +
                  Setting("datalog_files_directory", "/logs"));
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.Settings().calibration_files_directory, "/cal/");
    EXPECT_EQ(f.session.Settings().ecuflash_definition_files_directory, "C:\\defs\\"); // backslash accepted
    EXPECT_EQ(f.session.Settings().datalog_files_directory, "/logs/");
    EXPECT_THAT(f.Text(f.paths.config_file), HasSubstr(R"(data="/cal/")"));
}

TEST(ConfigSessionInitialize, RewriteFailureAfterLoadIsAWarningNotAFailure)
{
    ConfigSessionFixture f;
    f.PutSettings(Setting("calibration_files_directory", "/cal"));
    f.file_repository.write_errors[f.paths.config_file] = Error{ErrorKind::kInternal, "disk full"};

    ASSERT_THAT(f.Initialize(), IsOk());

    EXPECT_TRUE(HasLog(f.events, LogLevel::kWarning, f.paths.config_file));
    EXPECT_TRUE(HasLog(f.events, LogLevel::kWarning, "disk full"));
    EXPECT_EQ(f.session.Settings().calibration_files_directory, "/cal"); // unnormalized: nothing was written
}

TEST(ConfigSessionInitialize, MissingSettingsFileFailsNamingIt)
{
    ConfigSessionFixture f;
    f.file_repository.files.erase(f.paths.config_file);
    EXPECT_THAT(f.Initialize(), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr(f.paths.config_file)));
    EXPECT_FALSE(f.session.Initialized());
}

TEST(ConfigSessionInitialize, MalformedSettingsFileFailsNamingIt)
{
    ConfigSessionFixture f;
    f.Put(f.paths.config_file, "<config");
    EXPECT_THAT(f.Initialize(), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr(f.paths.config_file)));
}

TEST(ConfigSessionInitialize, AStaleProtocolsFileIsIgnored)
{
    ConfigSessionFixture f;
    f.Put(f.paths.config_files_directory + "protocols.cfg", "<config><protocols/><car_models/></config>");
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.Vehicles().size(), 3U);
    EXPECT_EQ(f.file_repository.ReadCount(f.paths.config_files_directory + "protocols.cfg"), 0);
}

TEST(ConfigSessionInitialize, ProvisioningFailureCarriesThePathAndReason)
{
    ConfigSessionFixture f;
    f.file_system.create_directory_error = Error{ErrorKind::kInternal, "permission denied"};
    EXPECT_THAT(f.Initialize(),
                IsErrWith(ErrorKind::kInternal, AllOf(HasSubstr("/root"), HasSubstr("permission denied"))));
}

TEST(ConfigSessionInitialize, FailedInitializationExposesNothing)
{
    ConfigSessionFixture f;
    f.Put(f.paths.config_file, "<config");
    ASSERT_FALSE(f.Initialize().has_value());

    EXPECT_TRUE(f.session.Vehicles().empty());
    EXPECT_FALSE(f.session.SelectedRow().has_value());
    EXPECT_EQ(f.session.SelectedVehicle(), nullptr);
    const std::size_t writes = f.file_repository.write_calls.size();
    EXPECT_FALSE(f.session.Save().has_value());
    EXPECT_EQ(f.file_repository.write_calls.size(), writes);
}

TEST(ConfigSessionInitialize, AFailedReinitializationDropsTheEarlierState)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    f.Put(f.paths.config_file, "<config");
    ASSERT_FALSE(f.Initialize().has_value());
    EXPECT_FALSE(f.session.Initialized());
    EXPECT_TRUE(f.session.Vehicles().empty());
}

TEST(ConfigSessionInitialize, AccessBeforeInitializationIsChecked)
{
    ConfigSessionFixture f;
    EXPECT_FALSE(f.session.Initialized());
    EXPECT_TRUE(f.session.Vehicles().empty());
    EXPECT_THAT(f.session.SelectedRow(), IsErr(ErrorKind::kInternal));
    EXPECT_EQ(f.session.SelectedVehicle(), nullptr);
    EXPECT_THAT(f.session.Save(), IsErr(ErrorKind::kInternal));
}

// --- saved vehicle --------------------------------------------------------

class UnusableSavedVehicle : public ::testing::TestWithParam<std::string>
{
};

TEST_P(UnusableSavedVehicle, SelectsNothingAndForgetsIt)
{
    ConfigSessionFixture f;
    f.PutSettings(Setting("vehicle_id", GetParam()));
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.SelectedVehicle(), nullptr);
    EXPECT_THAT(f.session.SelectedRow(), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(f.session.Settings().selected_vehicle_id.empty());
}

INSTANTIATE_TEST_SUITE_P(ConfigSessionInitialize, UnusableSavedVehicle,
                         ::testing::Values("", "0", "subaru-impreza-v9", "SUBARU-IMPREZA-V1"));

TEST(ConfigSessionInitialize, ALegacyProtocolIdSelectsNothing)
{
    ConfigSessionFixture f;
    f.PutSettings(Setting("protocol_id", "1"));
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.SelectedVehicle(), nullptr);
}

TEST(ConfigSessionInitialize, ValidSavedIdIsKept)
{
    ConfigSessionFixture f;
    f.PutSettings(Setting("vehicle_id", "subaru-forester-v3"));
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(*f.session.SelectedRow(), 2U);
    EXPECT_EQ(f.session.SelectedVehicle()->model, "Forester");
}

TEST(ConfigSessionInitialize, RestoringTheSavedRowKeepsSavedTransports)
{
    ConfigSessionFixture f;
    // Row 1's protocol defaults would be K-Line / K-Line / MUT_DMA.
    f.PutSettings(Setting("vehicle_id", "mitsubishi-colt-v2") + Setting("flash_transport", "CAN") +
                  Setting("log_transport", "J2534") + Setting("log_protocol", "SSM"));
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.Settings().selected_flash_transport, "CAN");
    EXPECT_EQ(f.session.Settings().selected_log_transport, "J2534");
    EXPECT_EQ(f.session.Settings().selected_log_protocol, "SSM");
}

// --- vehicle records ------------------------------------------------------

TEST(ConfigSessionVehicles, KeepCatalogOrder)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    const auto vehicles = f.session.Vehicles();
    ASSERT_EQ(vehicles.size(), 3U);
    EXPECT_EQ(vehicles[0].model, "Impreza");
    EXPECT_EQ(vehicles[1].model, "Colt");
    EXPECT_EQ(vehicles[2].model, "Forester");
}

TEST(ConfigSessionVehicles, SharedProtocolRowsPointAtTheSameEntry)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.Vehicles()[0].protocol, f.session.Vehicles()[2].protocol);
    EXPECT_EQ(f.session.Vehicles()[0].protocol->description, "Protocol A");
}

// --- paths ----------------------------------------------------------------

TEST(ConfigSessionPaths, EffectivePathsStartAsTheProvisionedPaths)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.EffectivePaths(), f.session.ProvisionedPaths());
}

TEST(ConfigSessionPaths, EditingConfigurableDirectoriesMovesOnlyThose)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    const ConfigPaths provisioned = f.session.ProvisionedPaths();

    f.session.Settings().calibration_files_directory = "/cal/";
    f.session.Settings().datalog_files_directory = "/logs/";

    ConfigPaths expected = provisioned;
    expected.calibration_files_directory = "/cal/";
    expected.datalog_files_directory = "/logs/";
    EXPECT_EQ(f.session.EffectivePaths(), expected);
    EXPECT_EQ(f.session.ProvisionedPaths(), provisioned);
}

TEST(ConfigSessionPaths, EmptyConfigurableDirectoryFallsBackToProvisioned)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    f.session.Settings().calibration_files_directory.clear();
    EXPECT_EQ(f.session.EffectivePaths().calibration_files_directory, f.paths.calibration_files_directory);
}

TEST(ConfigSessionPaths, DefinitionSearchSettingsAreNotResourceLocations)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    f.session.Settings().ecuflash_definition_files_directory = "/defs/";
    EXPECT_EQ(f.session.EffectivePaths().definition_files_directory, f.paths.definition_files_directory);
}

// --- saving ---------------------------------------------------------------

TEST(ConfigSessionSave, NormalizesAndUpdatesTheInMemorySettings)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    f.session.Settings().calibration_files_directory = "/new";
    f.session.Settings().serial_port = "COM3";

    ASSERT_THAT(f.session.Save(), IsOk());

    EXPECT_EQ(f.session.Settings().calibration_files_directory, "/new/");
    EXPECT_THAT(f.Text(f.paths.config_file), HasSubstr(R"(data="COM3")"));
}

TEST(ConfigSessionSave, FailureKeepsEditsAndNamesTheFile)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    f.session.Settings().calibration_files_directory = "/new";
    f.session.Settings().serial_port = "COM3";
    f.file_repository.write_errors[f.paths.config_file] = Error{ErrorKind::kInvalidConfig, "cannot open file"};

    EXPECT_THAT(f.session.Save(), IsErrWith(ErrorKind::kInvalidConfig,
                                            AllOf(HasSubstr(f.paths.config_file), HasSubstr("cannot open file"))));
    EXPECT_EQ(f.session.Settings().calibration_files_directory, "/new");
    EXPECT_EQ(f.session.Settings().serial_port, "COM3");
}

TEST(ConfigSessionSave, EveryOtherSettingRoundTripsThroughARestart)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    f.session.Settings().serial_port = "COM3";
    f.session.Settings().selected_flash_transport = "CAN";
    f.session.Settings().romraider_definition_files = {"r1.xml", "r2.xml"};
    f.session.Settings().calibration_files_directory = "/cal/";
    ASSERT_THAT(f.session.Save(), IsOk());

    ASSERT_THAT(f.Initialize(), IsOk()); // a restart over the same stores
    EXPECT_EQ(f.session.Settings().serial_port, "COM3");
    EXPECT_EQ(f.session.Settings().selected_flash_transport, "CAN");
    EXPECT_EQ(f.session.Settings().romraider_definition_files, (std::vector<std::string>{"r1.xml", "r2.xml"}));
    EXPECT_EQ(f.session.Settings().calibration_files_directory, "/cal/");
}

TEST(ConfigSessionSave, ASelectionSurvivesARestart)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    ASSERT_THAT(f.session.SelectRow(2), IsOk());
    ASSERT_THAT(f.session.Save(), IsOk());

    ASSERT_THAT(f.Initialize(), IsOk());

    ASSERT_NE(f.session.SelectedVehicle(), nullptr);
    EXPECT_EQ(f.session.SelectedVehicle()->id, "subaru-forester-v3");
}

TEST(ConfigSessionSave, DatalogDirectoryRoundTrips)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    f.session.Settings().datalog_files_directory = "/elsewhere/";
    ASSERT_THAT(f.session.Save(), IsOk());
    EXPECT_THAT(f.Text(f.paths.config_file), HasSubstr(R"(name="datalog_files_directory")"));

    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.Settings().datalog_files_directory, "/elsewhere/");
}

// --- selection ------------------------------------------------------------

TEST(ConfigSessionSelect, RowChangesTheSavedRowAndLogProtocolOnly)
{
    ConfigSessionFixture f;
    f.PutSettings(Setting("flash_transport", "CAN") + Setting("log_transport", "J2534") +
                  Setting("log_protocol", "SSM"));
    ASSERT_THAT(f.Initialize(), IsOk());

    ASSERT_THAT(f.session.SelectRow(1), IsOk());

    EXPECT_EQ(f.session.Settings().selected_vehicle_id, "mitsubishi-colt-v2");
    EXPECT_EQ(f.session.Settings().selected_log_protocol, "MUT_DMA");
    EXPECT_EQ(f.session.Settings().selected_flash_transport, "CAN");
    EXPECT_EQ(f.session.Settings().selected_log_transport, "J2534");
    EXPECT_EQ(f.session.SelectedVehicle()->model, "Colt");
}

TEST(ConfigSessionSelect, InvalidRowFailsWithoutChangingSettings)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    const AppConfig before = f.session.Settings();

    EXPECT_THAT(f.session.SelectRow(3), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(f.session.Settings(), before);
}

TEST(ConfigSessionSelect, BeforeInitializationFails)
{
    ConfigSessionFixture f;
    EXPECT_THAT(f.session.SelectRow(0), IsErr(ErrorKind::kInternal));
    EXPECT_FALSE(f.session.SelectByProtocolName("proto_a"));
}

TEST(ConfigSessionSelect, SelectionDoesNotSave)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    const std::size_t writes = f.file_repository.write_calls.size();
    ASSERT_THAT(f.session.SelectRow(2), IsOk());
    EXPECT_TRUE(f.session.SelectByProtocolName("proto_b"));
    EXPECT_EQ(f.file_repository.write_calls.size(), writes);
}

TEST(ConfigSessionSelect, ProtocolNameUsesTheLastMatchingRow)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());

    EXPECT_TRUE(f.session.SelectByProtocolName("proto_a")); // rows 0 and 2

    EXPECT_EQ(*f.session.SelectedRow(), 2U);
    EXPECT_EQ(f.session.Settings().selected_log_protocol, "SSM");
}

TEST(ConfigSessionSelect, UnmatchedProtocolNameChangesNothing)
{
    ConfigSessionFixture f;
    f.PutSettings(Setting("vehicle_id", "mitsubishi-colt-v2") + Setting("log_protocol", "CDBG"));
    ASSERT_THAT(f.Initialize(), IsOk());
    const AppConfig before = f.session.Settings();

    EXPECT_FALSE(f.session.SelectByProtocolName("no_such_protocol"));
    EXPECT_EQ(f.session.Settings(), before);
}

TEST(ConfigSessionSelect, AliasFindsTheFirstVehicleOfItsProtocol)
{
    ConfigSessionFixture f;
    EXPECT_EQ(f.session.VehicleForAlias("alias_a"), nullptr); // not initialized
    ASSERT_THAT(f.Initialize(), IsOk());
    EXPECT_EQ(f.session.VehicleForAlias("alias_a"), &f.session.Vehicles()[0]);
    EXPECT_EQ(f.session.VehicleForAlias("alias_b"), &f.session.Vehicles()[1]);
    EXPECT_EQ(f.session.VehicleForAlias("absent"), nullptr);
}

TEST(ConfigSessionSelect, FindProtocolNamesACatalogProtocolOnceInitialized)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    const fastecu::config::ProtocolSpec *first = f.session.Vehicles()[0].protocol;

    EXPECT_EQ(f.session.FindProtocol(first->name), first);
    EXPECT_EQ(f.session.FindProtocol("absent"), nullptr);
    ConfigSessionFixture uninitialized;
    EXPECT_EQ(uninitialized.session.FindProtocol(first->name), nullptr);
}

// --- built-in catalog -----------------------------------------------------

TEST(ConfigSessionBuiltin, TheMutDmaVehicleLogsMutDmaAndOffersNoFlashOperation)
{
    ConfigSessionFixture f;
    f.catalog = BuiltinCatalog();
    ASSERT_THAT(f.Initialize(), IsOk());
    const auto row = BuiltinCatalog().FindVehicle("mitsubishi-unknown-unk-ecu-unk--mitsu-ecu-m32r-kline-mut-dma");
    ASSERT_TRUE(row.has_value());

    ASSERT_THAT(f.session.SelectRow(*row), IsOk());

    EXPECT_EQ(f.session.Settings().selected_log_protocol, "MUT_DMA");
    const auto& protocol = *f.session.SelectedVehicle()->protocol;
    EXPECT_FALSE(protocol.read);
    EXPECT_FALSE(protocol.test_write);
    EXPECT_FALSE(protocol.write);
}

} // namespace
