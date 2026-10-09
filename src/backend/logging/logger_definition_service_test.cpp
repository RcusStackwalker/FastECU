#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/logging/logger_definition_service.h"

#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_resource_bundle.h"

namespace
{

using fastecu::logging::LoggerDefinitionService;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
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

std::vector<std::uint8_t> BytesOf(std::string_view text)
{
    return {text.begin(), text.end()};
}

class LoggerDefinitionServiceTest : public ::testing::Test
{
  protected:
    fastecu::InMemoryFileRepository repository_;
    fastecu::InMemoryResourceBundle bundle_;
    fastecu::InMemoryAtomicFileWriter writer_;

    LoggerDefinitionService Service()
    {
        return LoggerDefinitionService(repository_, bundle_, writer_);
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
    ASSERT_THAT(Service().ResolveDefinitionHandle("configured.xml", "CDBG", "/home/u/.FastECU/"),
                fastecu::testing::IsOkAnd("configured.xml"));
}

TEST_F(LoggerDefinitionServiceTest, PrefersTheConfigDirCdbgExampleWhenPresent)
{
    repository_.files["/home/u/.FastECU/logger_cdbg_example.xml"] = BytesOf(kDefinition);

    ASSERT_THAT(Service().ResolveDefinitionHandle("", "CDBG", "/home/u/.FastECU/"),
                fastecu::testing::IsOkAnd("/home/u/.FastECU/logger_cdbg_example.xml"));
}

TEST_F(LoggerDefinitionServiceTest, FallsBackToTheBundledCdbgExample)
{
    // Config-dir file absent; the bundled resource is the only source.
    ASSERT_THAT(Service().ResolveDefinitionHandle("", "CDBG", "/home/u/.FastECU/"),
                fastecu::testing::IsOkAnd(":/config/logger_cdbg_example.xml"));
}

TEST_F(LoggerDefinitionServiceTest, LeavesTheHandleEmptyForNonCdbgProtocols)
{
    const auto handle = Service().ResolveDefinitionHandle("", "SSM", "/home/u/.FastECU/");
    ASSERT_THAT(handle, fastecu::testing::IsOk());
    EXPECT_THAT(*handle, ::testing::IsEmpty());
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

TEST_F(LoggerDefinitionServiceTest, LoadsAnExistingSelectionWithoutWriting)
{
    repository_.files["logger.cfg"] = BytesOf(kConfWithEcu);
    const auto definition = fastecu::logging::LoggerDefinition{};

    const auto selection =
        Service().LoadOrInitializeSelection("logger.cfg", "ECUID1", fastecu::logging::DefaultSelection(definition));
    ASSERT_THAT(selection, fastecu::testing::IsOk());
    EXPECT_THAT(selection->gauge_ids, ElementsAre("P2"));
    EXPECT_TRUE(writer_.replace_calls.empty()) << "reading must not write";
}

TEST_F(LoggerDefinitionServiceTest, InitializesAndPersistsWhenTheEcuIsAbsent)
{
    repository_.files["logger.cfg"] = BytesOf("<config><logger/></config>");
    const auto parsed = fastecu::logging::ParseLoggerDefinition(
        bytes::ByteView(reinterpret_cast<const bytes::Byte *>(kDefinition.data()), kDefinition.size()), "logger.xml");
    ASSERT_THAT(parsed, fastecu::testing::IsOk());

    const auto selection =
        Service().LoadOrInitializeSelection("logger.cfg", "NEWECU", fastecu::logging::DefaultSelection(*parsed));
    ASSERT_THAT(selection, fastecu::testing::IsOk());
    // Enabled-only walk: P1 is enabled, P2 is not.
    EXPECT_THAT(selection->gauge_ids, ElementsAre("P1"));
    ASSERT_THAT(writer_.replace_calls, SizeIs(1)) << "the default must be persisted";
    EXPECT_EQ(writer_.replace_calls.at(0).handle, "logger.cfg");
}

TEST_F(LoggerDefinitionServiceTest, InitializingReadsTheConfExactlyOnce)
{
    repository_.files["logger.cfg"] = BytesOf("<config><logger/></config>");

    ASSERT_THAT(Service().LoadOrInitializeSelection("logger.cfg", "NEWECU", fastecu::logging::LoggerSelection{}),
                fastecu::testing::IsOk());
    // load_or_initialize_selection shares load_selection's single read rather
    // than re-reading before it writes: no TOCTOU window inside the service.
    EXPECT_EQ(repository_.ReadCount("logger.cfg"), 1);
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

TEST_F(LoggerDefinitionServiceTest, InitializePropagatesAnUnreadableHandle)
{
    ASSERT_THAT(Service().LoadOrInitializeSelection("missing.cfg", "NEWECU", fastecu::logging::LoggerSelection{}),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_TRUE(writer_.replace_calls.empty()) << "a failed read must not write";
}

TEST_F(LoggerDefinitionServiceTest, InitializePropagatesAWriteSelectionFailure)
{
    // A conf whose root is not <config> cannot be extended without emitting
    // two document elements; write_selection refuses, and the service must
    // surface that rather than replace the file.
    repository_.files["logger.cfg"] = BytesOf("<notconfig/>");

    ASSERT_THAT(Service().LoadOrInitializeSelection("logger.cfg", "NEWECU", fastecu::logging::LoggerSelection{}),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_TRUE(writer_.replace_calls.empty()) << "a refused write must not reach the writer";
}

TEST_F(LoggerDefinitionServiceTest, InitializePropagatesAReplaceFailure)
{
    repository_.files["logger.cfg"] = BytesOf("<config><logger/></config>");
    writer_.replace_error = fastecu::Error{fastecu::ErrorKind::kInternal, "disk full"};

    const auto selection =
        Service().LoadOrInitializeSelection("logger.cfg", "NEWECU", fastecu::logging::LoggerSelection{});
    ASSERT_THAT(selection, fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
    EXPECT_THAT(selection.error().detail, HasSubstr("disk full"));
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
    EXPECT_THAT(status.error().detail, HasSubstr("read-only volume"));
}

} // namespace

TEST_F(LoggerDefinitionServiceTest, PersistsExplicitFallbackIncludingUnresolvedIds)
{
    repository_.files["logger.cfg"] = BytesOf("<config><logger/></config>");
    const fastecu::logging::LoggerSelection fallback{
        .protocol = "MUT_DMA", .gauge_ids = {"unknown", "rpm"}, .lower_panel_ids = {"rpm"}};
    const auto result = Service().LoadOrInitializeSelection("logger.cfg", "NEWECU", fallback);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(*result, fallback);
    const auto stored = fastecu::logging::ReadSelection(writer_.files.at("logger.cfg"), "NEWECU", "logger.cfg");
    ASSERT_THAT(stored, fastecu::testing::IsOk());
    ASSERT_TRUE(stored->has_value());
    EXPECT_EQ(**stored, fallback);
}
