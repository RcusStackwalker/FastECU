#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/logging/logging_selection_use_case.h"

#include <string>
#include <string_view>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/logging/logger_definition_service.h"
#include "src/backend/logging/logger_model.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_resource_bundle.h"

namespace
{

using fastecu::logging::DefinitionSource;
using fastecu::logging::InstallFailure;
using fastecu::logging::LoggerDefinition;
using fastecu::logging::LoggerSelection;
using fastecu::logging::LoggingSelectionUseCase;
using fastecu::logging::RestoreOutcome;
using ::testing::ElementsAre;
using ::testing::SizeIs;

constexpr std::string_view kDefinition =
    R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1" enabled="1"><address>0x1</address></parameter>
  <parameter id="P2" enabled="0"><address>0x2</address></parameter>
</parameters><switches><switch id="S1"/></switches></protocol></protocols></logger>)";

constexpr std::string_view kConfWithEcu =
    R"(<config><logger><ecu id="ECUID1"><protocol id="SSM"><parameters>
  <gauges><parameter id="P2" name=""/></gauges>
  <lower_panel><parameter id="P2" name=""/></lower_panel>
</parameters><switches><switch id="S1" name=""/></switches></protocol></ecu></logger></config>)";

constexpr std::string_view kEmptyConf = "<config><logger/></config>";

std::vector<std::uint8_t> BytesOf(std::string_view text)
{
    return {text.begin(), text.end()};
}

// Every ID list empty, protocol retained.
LoggerSelection Cleared(std::string protocol)
{
    return {.protocol = std::move(protocol)};
}

class LoggingSelectionUseCaseTest : public ::testing::Test
{
  protected:
    fastecu::InMemoryFileRepository repository_;
    fastecu::InMemoryResourceBundle bundle_;
    fastecu::InMemoryAtomicFileWriter writer_;
    fastecu::logging::LoggerDefinitionService definitions_{repository_, bundle_, writer_};
    fastecu::logging::LoggerModel model_;

    LoggingSelectionUseCase UseCase()
    {
        return LoggingSelectionUseCase(definitions_, model_);
    }

    // Installs a definition with one SSM parameter and a selection the
    // use case must leave alone when it fails.
    void PreinstallKeptState()
    {
        model_.InstallDefinition({.parameters = {{.protocol = "SSM", .id = "kept", .enabled = true}}});
        model_.SetSelection({.protocol = "SSM", .gauge_ids = {"kept"}});
    }
};

using LoggingSelectionUseCaseInstall = LoggingSelectionUseCaseTest;

TEST_F(LoggingSelectionUseCaseInstall, InstallsTheConfiguredDefinitionAndResetsToItsDefault)
{
    repository_.files["defs.xml"] = BytesOf(kDefinition);
    model_.SetSelection({.protocol = "SSM", .gauge_ids = {"stale"}});

    const auto installed = UseCase().InstallDefinition({.configured_handle = "defs.xml", .log_protocol = "SSM"});

    ASSERT_TRUE(installed.has_value());
    EXPECT_EQ(installed->resolved_handle, "defs.xml");
    EXPECT_FALSE(installed->bundled_handle_used);
    EXPECT_THAT(model_.Definition().parameters, SizeIs(2));
    EXPECT_EQ(model_.Selection(), model_.DefaultSelection("SSM"));
}

TEST_F(LoggingSelectionUseCaseInstall, ReportsTheBundledHandleWhenNoneIsConfigured)
{
    repository_.files["/home/u/.FastECU/logger_cdbg_example.xml"] = BytesOf(kDefinition);

    const auto installed = UseCase().InstallDefinition(
        {.configured_handle = "", .log_protocol = "CDBG", .config_files_directory = "/home/u/.FastECU/"});

    ASSERT_TRUE(installed.has_value());
    EXPECT_TRUE(installed->bundled_handle_used);
    EXPECT_EQ(installed->resolved_handle, "/home/u/.FastECU/logger_cdbg_example.xml");
}

TEST_F(LoggingSelectionUseCaseInstall, BundledHandleIsReportedEvenWhenItsLoadFails)
{
    // Resolution only probes the user copy for readability; a malformed one fails to parse.
    repository_.files["/home/u/.FastECU/logger_cdbg_example.xml"] = BytesOf("<logger><protocols>");

    const auto installed = UseCase().InstallDefinition(
        {.configured_handle = "", .log_protocol = "CDBG", .config_files_directory = "/home/u/.FastECU/"});

    ASSERT_FALSE(installed.has_value());
    EXPECT_EQ(installed.error().step, InstallFailure::Step::kLoad);
    EXPECT_TRUE(installed.error().bundled_handle_used);
    EXPECT_EQ(installed.error().handle, "/home/u/.FastECU/logger_cdbg_example.xml");
}

TEST_F(LoggingSelectionUseCaseInstall, ConfiguredHandleIsNeverReportedAsBundled)
{
    repository_.files["defs.xml"] = BytesOf(kDefinition);

    const auto installed = UseCase().InstallDefinition({.configured_handle = "defs.xml", .log_protocol = "CDBG"});

    ASSERT_TRUE(installed.has_value());
    EXPECT_FALSE(installed->bundled_handle_used);
}

TEST_F(LoggingSelectionUseCaseInstall, UnreadableDefinitionFailsAtLoadAndLeavesTheModelUntouched)
{
    PreinstallKeptState();
    const auto definition = model_.Definition();
    const auto selection = model_.Selection();

    const auto installed = UseCase().InstallDefinition({.configured_handle = "missing.xml", .log_protocol = "SSM"});

    ASSERT_FALSE(installed.has_value());
    EXPECT_EQ(installed.error().step, InstallFailure::Step::kLoad);
    EXPECT_EQ(installed.error().handle, "missing.xml");
    EXPECT_EQ(model_.Definition(), definition);
    EXPECT_EQ(model_.Selection(), selection);
}

TEST_F(LoggingSelectionUseCaseInstall, EmptyHandleForANonCdbgProtocolInstallsNothing)
{
    PreinstallKeptState();
    const auto selection = model_.Selection();

    // Resolution yields an empty handle for non-CDBG protocols; loading it fails.
    const auto installed = UseCase().InstallDefinition({.configured_handle = "", .log_protocol = "SSM"});

    ASSERT_FALSE(installed.has_value());
    EXPECT_EQ(installed.error().step, InstallFailure::Step::kLoad);
    EXPECT_EQ(installed.error().handle, "");
    EXPECT_EQ(model_.Selection(), selection);
}

class LoggingSelectionUseCaseRestore : public LoggingSelectionUseCaseTest
{
  protected:
    void SetUp() override
    {
        repository_.files["defs.xml"] = BytesOf(kDefinition);
        ASSERT_TRUE(UseCase().InstallDefinition({.configured_handle = "defs.xml", .log_protocol = "SSM"}).has_value());
    }
};

TEST_F(LoggingSelectionUseCaseRestore, UnreadableConfKeepsTheCurrentSelection)
{
    model_.SetSelection({.protocol = "SSM", .gauge_ids = {"old"}});

    EXPECT_THAT(UseCase().RestorePendingSelection("conf.xml", "ECUID1", "SSM"),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_EQ(model_.Selection(), (LoggerSelection{.protocol = "SSM", .gauge_ids = {"old"}}));
}

TEST_F(LoggingSelectionUseCaseRestore, AppliesTheEcusStoredEntry)
{
    repository_.files["conf.xml"] = BytesOf(kConfWithEcu);

    EXPECT_THAT(UseCase().RestorePendingSelection("conf.xml", "ECUID1", "SSM"),
                fastecu::testing::IsOkAnd(RestoreOutcome::kRestored));
    EXPECT_THAT(model_.Selection().gauge_ids, ElementsAre("P2"));
    EXPECT_THAT(model_.Selection().switch_ids, ElementsAre("S1"));
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(LoggingSelectionUseCaseRestore, InitializesAndPersistsTheDefaultForAnAbsentEcu)
{
    repository_.files["conf.xml"] = BytesOf(kConfWithEcu);

    EXPECT_THAT(UseCase().RestorePendingSelection("conf.xml", "OTHER", "SSM"),
                fastecu::testing::IsOkAnd(RestoreOutcome::kInitialized));
    EXPECT_EQ(model_.Selection(), model_.DefaultSelection("SSM"));
    ASSERT_THAT(writer_.replace_calls, SizeIs(1));
    repository_.files["conf.xml"] = writer_.replace_calls.front().data;
    const auto stored = definitions_.LoadSelection("conf.xml", "OTHER");
    ASSERT_THAT(stored, fastecu::testing::IsOk());
    EXPECT_EQ(*stored, model_.Selection());
}

TEST_F(LoggingSelectionUseCaseTest, SwitchOnlyDefinitionStillInitializes)
{
    model_.InstallDefinition({.switches = {{.protocol = "SSM", .id = "S1", .enabled = true}}});
    model_.SetSelection({.protocol = "SSM"});
    repository_.files["conf.xml"] = BytesOf(kEmptyConf);

    EXPECT_THAT(UseCase().RestorePendingSelection("conf.xml", "ECUID1", "SSM"),
                fastecu::testing::IsOkAnd(RestoreOutcome::kInitialized));
    EXPECT_THAT(model_.Selection().switch_ids, ElementsAre("S1"));
    EXPECT_THAT(writer_.replace_calls, SizeIs(1));
}

TEST_F(LoggingSelectionUseCaseTest, EmptyDefinitionClearsWithoutPersisting)
{
    model_.SetSelection({.protocol = "SSM", .lower_panel_ids = {"stale"}});
    repository_.files["conf.xml"] = BytesOf(kEmptyConf);

    EXPECT_THAT(UseCase().RestorePendingSelection("conf.xml", "ECUID1", "SSM"),
                fastecu::testing::IsOkAnd(RestoreOutcome::kClearedNoDefinition));
    EXPECT_EQ(model_.Selection(), Cleared("SSM"));
    EXPECT_TRUE(writer_.replace_calls.empty());
    EXPECT_EQ(repository_.files.at("conf.xml"), BytesOf(kEmptyConf));
}

TEST_F(LoggingSelectionUseCaseTest, ClearingKeepsTheModelsProtocolNotTheRequestedOne)
{
    model_.SetSelection({.protocol = "SSM", .lower_panel_ids = {"stale"}});
    repository_.files["conf.xml"] = BytesOf(kEmptyConf);

    ASSERT_THAT(UseCase().RestorePendingSelection("conf.xml", "ECUID1", "MUT"), fastecu::testing::IsOk());
    EXPECT_EQ(model_.Selection().protocol, "SSM");
}

TEST_F(LoggingSelectionUseCaseRestore, InitializeWriteFailureLeavesTheSelectionCleared)
{
    model_.SetSelection({.protocol = "SSM", .gauge_ids = {"old"}});
    repository_.files["conf.xml"] = BytesOf(kEmptyConf);
    writer_.replace_error = fastecu::Error{.kind = fastecu::ErrorKind::kInternal, .detail = "disk full"};

    EXPECT_THAT(UseCase().RestorePendingSelection("conf.xml", "OTHER", "SSM"),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
    EXPECT_EQ(model_.Selection(), Cleared("SSM"));
}

using LoggingSelectionUseCaseSave = LoggingSelectionUseCaseTest;

TEST_F(LoggingSelectionUseCaseSave, AppliesThenPersistsTheSelection)
{
    repository_.files["conf.xml"] = BytesOf(kConfWithEcu);
    const LoggerSelection edited{.protocol = "SSM", .gauge_ids = {"P1"}};

    ASSERT_THAT(UseCase().SavePendingSelection("conf.xml", "ECUID1", edited), fastecu::testing::IsOk());
    EXPECT_EQ(model_.Selection(), edited);
    ASSERT_THAT(writer_.replace_calls, SizeIs(1));
    repository_.files["conf.xml"] = writer_.replace_calls.front().data;
    const auto stored = definitions_.LoadSelection("conf.xml", "ECUID1");
    ASSERT_THAT(stored, fastecu::testing::IsOk());
    EXPECT_EQ(*stored, edited);
}

TEST_F(LoggingSelectionUseCaseSave, SaveFailureKeepsTheEditedSelectionInTheModel)
{
    const LoggerSelection edited{.protocol = "SSM", .gauge_ids = {"P1"}};

    EXPECT_THAT(UseCase().SavePendingSelection("conf.xml", "ECUID1", edited),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_EQ(model_.Selection(), edited);
}

} // namespace
