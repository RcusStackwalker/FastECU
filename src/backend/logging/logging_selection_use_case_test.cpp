#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/logging/logging_selection_use_case.h"

#include <string>
#include <string_view>

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
using ::testing::SizeIs;

constexpr std::string_view kDefinition =
    R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1" enabled="1"><address>0x1</address></parameter>
  <parameter id="P2" enabled="0"><address>0x2</address></parameter>
</parameters><switches><switch id="S1"/></switches></protocol></protocols></logger>)";

std::vector<std::uint8_t> BytesOf(std::string_view text)
{
    return {text.begin(), text.end()};
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

} // namespace
