#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/logging/logger_definition_service.h"

#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_test_utils.h"
#include "src/backend/logging/testing/logger_xml_fixtures.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_file_system.h"

namespace
{

using fastecu::logging::LoggerDefinitionService;
using fastecu::logging::testing::kConfWithEcu;
using fastecu::logging::testing::kDefinition;
using test_bytes::BytesOf;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::SizeIs;

class LoggerDefinitionServiceTest : public ::testing::Test
{
  protected:
    fastecu::InMemoryFileSystem file_system_;
    fastecu::InMemoryFileRepository repository_;
    fastecu::InMemoryAtomicFileWriter writer_;

    LoggerDefinitionService Service()
    {
        return LoggerDefinitionService(file_system_, repository_, writer_);
    }
};

TEST_F(LoggerDefinitionServiceTest, LoadsAndParsesTheConfiguredHandle)
{
    repository_.files["logger.xml"] = BytesOf(kDefinition);

    const auto definition = Service().LoadDefinition("logger.xml");
    ASSERT_THAT(definition, fastecu::testing::IsOk());
    EXPECT_THAT(definition->parameters, SizeIs(2));
    EXPECT_THAT(definition->switches, SizeIs(1));
}

TEST_F(LoggerDefinitionServiceTest, PropagatesAReadFailure)
{
    ASSERT_THAT(Service().LoadDefinition("missing.xml"), fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST_F(LoggerDefinitionServiceTest, ResolvesTheConfiguredHandleUnchanged)
{
    EXPECT_EQ(Service().ResolveDefinitionHandle("configured.xml", "CDBG", "/home/u/.FastECU/"), "configured.xml");
}

TEST_F(LoggerDefinitionServiceTest, PrefersTheConfigDirCdbgExampleWhenPresent)
{
    file_system_.files["/home/u/.FastECU/logger_cdbg_example.xml"] = BytesOf(kDefinition);

    EXPECT_EQ(Service().ResolveDefinitionHandle("", "CDBG", "/home/u/.FastECU/"),
              "/home/u/.FastECU/logger_cdbg_example.xml");
}

TEST_F(LoggerDefinitionServiceTest, ChoosesAnExistingConfigDirCopyEvenWhenItCannotBeRead)
{
    // Exists but unreadable through the repository: it must still be chosen,
    // so loading it reports the failure instead of silently using the bundle.
    file_system_.files["/home/u/.FastECU/logger_cdbg_example.xml"] = BytesOf(kDefinition);

    const std::string handle = Service().ResolveDefinitionHandle("", "CDBG", "/home/u/.FastECU/");

    EXPECT_EQ(handle, "/home/u/.FastECU/logger_cdbg_example.xml");
    EXPECT_THAT(Service().LoadDefinition(handle), fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST_F(LoggerDefinitionServiceTest, FallsBackToTheBundledCdbgExample)
{
    // Config-dir file absent; the bundled resource is the only source.
    EXPECT_EQ(Service().ResolveDefinitionHandle("", "CDBG", "/home/u/.FastECU/"), ":/config/logger_cdbg_example.xml");
}

TEST_F(LoggerDefinitionServiceTest, LeavesTheHandleEmptyForNonCdbgProtocols)
{
    EXPECT_THAT(Service().ResolveDefinitionHandle("", "SSM", "/home/u/.FastECU/"), ::testing::IsEmpty());
}

TEST_F(LoggerDefinitionServiceTest, LoadSelectionReturnsTheStoredEntry)
{
    repository_.files["logger.cfg"] = BytesOf(kConfWithEcu);

    const auto stored = Service().LoadSelection("logger.cfg", "ECUID1");
    ASSERT_THAT(stored, fastecu::testing::IsOk());
    ASSERT_TRUE(stored->has_value()) << "ECUID1 has an <ecu> element";
    EXPECT_EQ((*stored)->protocol, "SSM");
    EXPECT_THAT((*stored)->gauge_ids, ElementsAre("P2"));
    EXPECT_THAT((*stored)->lower_panel_ids, ElementsAre("P2"));
    EXPECT_THAT((*stored)->switch_ids, ElementsAre("S1"));
    EXPECT_TRUE(writer_.replace_calls.empty()) << "reading must not write";
}

TEST_F(LoggerDefinitionServiceTest, LoadSelectionReturnsNulloptForAnAbsentEcuWithoutWriting)
{
    repository_.files["logger.cfg"] = BytesOf(kConfWithEcu);

    const auto stored = Service().LoadSelection("logger.cfg", "OTHERECU");
    ASSERT_THAT(stored, fastecu::testing::IsOk());
    EXPECT_FALSE(stored->has_value()) << "an absent ECU is not an error";
    // The no-definition-loaded caller relies on this: asking must not seed a
    // default the way load_or_initialize_selection does.
    EXPECT_TRUE(writer_.replace_calls.empty()) << "an absent ECU must not be initialized";
    EXPECT_EQ(repository_.files.at("logger.cfg"), BytesOf(kConfWithEcu));
}

TEST_F(LoggerDefinitionServiceTest, LoadSelectionPropagatesAnUnreadableHandle)
{
    ASSERT_THAT(Service().LoadSelection("missing.cfg", "ECUID1"),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(LoggerDefinitionServiceTest, LoadSelectionPropagatesAParseFailure)
{
    repository_.files["logger.cfg"] = BytesOf("<config><logger><ecu id=");

    ASSERT_THAT(Service().LoadSelection("logger.cfg", "ECUID1"),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST_F(LoggerDefinitionServiceTest, SaveSelectionReplacesTheFileAtomically)
{
    repository_.files["logger.cfg"] = BytesOf(kConfWithEcu);
    fastecu::logging::LoggerSelection selection;
    selection.protocol = "SSM";
    selection.gauge_ids = {"P9"};

    ASSERT_THAT(Service().SaveSelection("logger.cfg", "ECUID1", selection), fastecu::testing::IsOk());
    ASSERT_THAT(writer_.replace_calls, SizeIs(1));
    const auto& written = writer_.replace_calls.at(0).data;
    EXPECT_THAT(std::string(written.begin(), written.end()), HasSubstr("P9"));
}

TEST_F(LoggerDefinitionServiceTest, SaveSelectionPropagatesAnUnreadableHandle)
{
    ASSERT_THAT(Service().SaveSelection("missing.cfg", "ECUID1", fastecu::logging::LoggerSelection{}),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_TRUE(writer_.replace_calls.empty()) << "a failed read must not write";
}

TEST_F(LoggerDefinitionServiceTest, SaveSelectionPropagatesAWriteSelectionFailure)
{
    repository_.files["logger.cfg"] = BytesOf("<notconfig/>");

    ASSERT_THAT(Service().SaveSelection("logger.cfg", "ECUID1", fastecu::logging::LoggerSelection{}),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_TRUE(writer_.replace_calls.empty()) << "a refused write must not reach the writer";
}

TEST_F(LoggerDefinitionServiceTest, SaveSelectionPropagatesAReplaceFailure)
{
    repository_.files["logger.cfg"] = BytesOf(kConfWithEcu);
    writer_.replace_error = fastecu::Error{fastecu::ErrorKind::kInternal, "read-only volume"};

    const auto status = Service().SaveSelection("logger.cfg", "ECUID1", fastecu::logging::LoggerSelection{});
    ASSERT_THAT(status, fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
    EXPECT_THAT(status, fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, HasSubstr("read-only volume")));
}

} // namespace

TEST_F(LoggerDefinitionServiceTest, SaveSelectionPersistsAMissingEcuIncludingUnresolvedIds)
{
    repository_.files["logger.cfg"] = BytesOf("<config><logger/></config>");
    const fastecu::logging::LoggerSelection fallback{
        .protocol = "MUT_DMA", .gauge_ids = {"unknown", "rpm"}, .lower_panel_ids = {"rpm"}};
    ASSERT_THAT(Service().SaveSelection("logger.cfg", "NEWECU", fallback), fastecu::testing::IsOk());
    const auto stored = fastecu::logging::ReadSelection(writer_.files.at("logger.cfg"), "NEWECU", "logger.cfg");
    ASSERT_THAT(stored, fastecu::testing::IsOk());
    ASSERT_TRUE(stored->has_value());
    EXPECT_EQ(**stored, fallback);
}
