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

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return {text.begin(), text.end()};
}

class LoggerDefinitionServiceTest : public ::testing::Test
{
  protected:
    fastecu::InMemoryFileRepository repository_;
    fastecu::InMemoryResourceBundle bundle_;
    fastecu::InMemoryAtomicFileWriter writer_;

    LoggerDefinitionService service()
    {
        return LoggerDefinitionService(repository_, bundle_, writer_);
    }
};

TEST_F(LoggerDefinitionServiceTest, LoadsAndParsesTheConfiguredHandle)
{
    repository_.files["logger.xml"] = bytes_of(kDefinition);

    const auto definition = service().load_definition("logger.xml");
    ASSERT_TRUE(definition.has_value()) << definition.error().detail;
    EXPECT_THAT(definition->parameters, SizeIs(2));
    EXPECT_THAT(definition->switches, SizeIs(1));
}

TEST_F(LoggerDefinitionServiceTest, PropagatesAReadFailure)
{
    const auto definition = service().load_definition("missing.xml");
    ASSERT_FALSE(definition.has_value());
    EXPECT_EQ(definition.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST_F(LoggerDefinitionServiceTest, ResolvesTheConfiguredHandleUnchanged)
{
    const auto handle = service().resolve_definition_handle(
        "configured.xml", "CDBG", "/home/u/.FastECU/");
    ASSERT_TRUE(handle.has_value()) << handle.error().detail;
    EXPECT_EQ(*handle, "configured.xml");
}

TEST_F(LoggerDefinitionServiceTest, PrefersTheConfigDirCdbgExampleWhenPresent)
{
    repository_.files["/home/u/.FastECU/logger_cdbg_example.xml"] =
        bytes_of(kDefinition);

    const auto handle = service().resolve_definition_handle("", "CDBG", "/home/u/.FastECU/");
    ASSERT_TRUE(handle.has_value()) << handle.error().detail;
    EXPECT_EQ(*handle, "/home/u/.FastECU/logger_cdbg_example.xml");
}

TEST_F(LoggerDefinitionServiceTest, FallsBackToTheBundledCdbgExample)
{
    // Config-dir file absent; the bundled resource is the only source.
    const auto handle = service().resolve_definition_handle("", "CDBG", "/home/u/.FastECU/");
    ASSERT_TRUE(handle.has_value()) << handle.error().detail;
    EXPECT_EQ(*handle, ":/config/logger_cdbg_example.xml");
}

TEST_F(LoggerDefinitionServiceTest, LeavesTheHandleEmptyForNonCdbgProtocols)
{
    const auto handle = service().resolve_definition_handle("", "SSM", "/home/u/.FastECU/");
    ASSERT_TRUE(handle.has_value()) << handle.error().detail;
    EXPECT_THAT(*handle, ::testing::IsEmpty());
}

TEST_F(LoggerDefinitionServiceTest, LoadSelectionReturnsTheStoredEntry)
{
    repository_.files["logger.cfg"] = bytes_of(kConfWithEcu);

    const auto stored = service().load_selection("logger.cfg", "ECUID1");
    ASSERT_TRUE(stored.has_value()) << stored.error().detail;
    ASSERT_TRUE(stored->has_value()) << "ECUID1 has an <ecu> element";
    EXPECT_EQ((*stored)->protocol, "SSM");
    EXPECT_THAT((*stored)->gauge_ids, ElementsAre("P2"));
    EXPECT_THAT((*stored)->lower_panel_ids, ElementsAre("P2"));
    EXPECT_THAT((*stored)->switch_ids, ElementsAre("S1"));
    EXPECT_TRUE(writer_.replace_calls.empty()) << "reading must not write";
}

TEST_F(LoggerDefinitionServiceTest, LoadSelectionReturnsNulloptForAnAbsentEcuWithoutWriting)
{
    repository_.files["logger.cfg"] = bytes_of(kConfWithEcu);

    const auto stored = service().load_selection("logger.cfg", "OTHERECU");
    ASSERT_TRUE(stored.has_value()) << stored.error().detail;
    EXPECT_FALSE(stored->has_value()) << "an absent ECU is not an error";
    // The no-definition-loaded caller relies on this: asking must not seed a
    // default the way load_or_initialize_selection does.
    EXPECT_TRUE(writer_.replace_calls.empty()) << "an absent ECU must not be initialized";
    EXPECT_EQ(repository_.files.at("logger.cfg"), bytes_of(kConfWithEcu));
}

TEST_F(LoggerDefinitionServiceTest, LoadSelectionPropagatesAnUnreadableHandle)
{
    const auto stored = service().load_selection("missing.cfg", "ECUID1");
    ASSERT_FALSE(stored.has_value());
    EXPECT_EQ(stored.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(LoggerDefinitionServiceTest, LoadSelectionPropagatesAParseFailure)
{
    repository_.files["logger.cfg"] = bytes_of("<config><logger><ecu id=");

    const auto stored = service().load_selection("logger.cfg", "ECUID1");
    ASSERT_FALSE(stored.has_value());
    EXPECT_EQ(stored.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST_F(LoggerDefinitionServiceTest, LoadsAnExistingSelectionWithoutWriting)
{
    repository_.files["logger.cfg"] = bytes_of(kConfWithEcu);
    const auto definition = fastecu::logging::LoggerDefinition{};

    const auto selection =
        service().load_or_initialize_selection("logger.cfg", "ECUID1", definition);
    ASSERT_TRUE(selection.has_value()) << selection.error().detail;
    EXPECT_THAT(selection->gauge_ids, ElementsAre("P2"));
    EXPECT_TRUE(writer_.replace_calls.empty()) << "reading must not write";
}

TEST_F(LoggerDefinitionServiceTest, InitializesAndPersistsWhenTheEcuIsAbsent)
{
    repository_.files["logger.cfg"] = bytes_of("<config><logger/></config>");
    const auto parsed = fastecu::logging::parse_logger_definition(
        bytes::ByteView(reinterpret_cast<const bytes::Byte *>(kDefinition.data()),
                        kDefinition.size()),
        "logger.xml");
    ASSERT_TRUE(parsed.has_value());

    const auto selection =
        service().load_or_initialize_selection("logger.cfg", "NEWECU", *parsed);
    ASSERT_TRUE(selection.has_value()) << selection.error().detail;
    // Enabled-only walk: P1 is enabled, P2 is not.
    EXPECT_THAT(selection->gauge_ids, ElementsAre("P1"));
    ASSERT_THAT(writer_.replace_calls, SizeIs(1)) << "the default must be persisted";
    EXPECT_EQ(writer_.replace_calls.at(0).handle, "logger.cfg");
}

TEST_F(LoggerDefinitionServiceTest, InitializingReadsTheConfExactlyOnce)
{
    repository_.files["logger.cfg"] = bytes_of("<config><logger/></config>");

    const auto selection = service().load_or_initialize_selection(
        "logger.cfg", "NEWECU", fastecu::logging::LoggerDefinition{});
    ASSERT_TRUE(selection.has_value()) << selection.error().detail;
    // load_or_initialize_selection shares load_selection's single read rather
    // than re-reading before it writes: no TOCTOU window inside the service.
    EXPECT_EQ(repository_.read_count("logger.cfg"), 1);
}

TEST_F(LoggerDefinitionServiceTest, SaveSelectionReplacesTheFileAtomically)
{
    repository_.files["logger.cfg"] = bytes_of(kConfWithEcu);
    fastecu::logging::LoggerSelection selection;
    selection.protocol = "SSM";
    selection.gauge_ids = {"P9"};

    const auto status = service().save_selection("logger.cfg", "ECUID1", selection);
    ASSERT_TRUE(status.has_value()) << status.error().detail;
    ASSERT_THAT(writer_.replace_calls, SizeIs(1));
    const auto& written = writer_.replace_calls.at(0).data;
    EXPECT_THAT(std::string(written.begin(), written.end()), HasSubstr("P9"));
}

TEST_F(LoggerDefinitionServiceTest, InitializePropagatesAnUnreadableHandle)
{
    const auto selection = service().load_or_initialize_selection(
        "missing.cfg", "NEWECU", fastecu::logging::LoggerDefinition{});
    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_TRUE(writer_.replace_calls.empty()) << "a failed read must not write";
}

TEST_F(LoggerDefinitionServiceTest, InitializePropagatesAWriteSelectionFailure)
{
    // A conf whose root is not <config> cannot be extended without emitting
    // two document elements; write_selection refuses, and the service must
    // surface that rather than replace the file.
    repository_.files["logger.cfg"] = bytes_of("<notconfig/>");

    const auto selection = service().load_or_initialize_selection(
        "logger.cfg", "NEWECU", fastecu::logging::LoggerDefinition{});
    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_TRUE(writer_.replace_calls.empty()) << "a refused write must not reach the writer";
}

TEST_F(LoggerDefinitionServiceTest, InitializePropagatesAReplaceFailure)
{
    repository_.files["logger.cfg"] = bytes_of("<config><logger/></config>");
    writer_.replace_error = fastecu::Error{fastecu::ErrorKind::Internal, "disk full"};

    const auto selection = service().load_or_initialize_selection(
        "logger.cfg", "NEWECU", fastecu::logging::LoggerDefinition{});
    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_THAT(selection.error().detail, HasSubstr("disk full"));
}

TEST_F(LoggerDefinitionServiceTest, SaveSelectionPropagatesAnUnreadableHandle)
{
    const auto status =
        service().save_selection("missing.cfg", "ECUID1", fastecu::logging::LoggerSelection{});
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_TRUE(writer_.replace_calls.empty()) << "a failed read must not write";
}

TEST_F(LoggerDefinitionServiceTest, SaveSelectionPropagatesAWriteSelectionFailure)
{
    repository_.files["logger.cfg"] = bytes_of("<notconfig/>");

    const auto status =
        service().save_selection("logger.cfg", "ECUID1", fastecu::logging::LoggerSelection{});
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_TRUE(writer_.replace_calls.empty()) << "a refused write must not reach the writer";
}

TEST_F(LoggerDefinitionServiceTest, SaveSelectionPropagatesAReplaceFailure)
{
    repository_.files["logger.cfg"] = bytes_of(kConfWithEcu);
    writer_.replace_error = fastecu::Error{fastecu::ErrorKind::Internal, "read-only volume"};

    const auto status =
        service().save_selection("logger.cfg", "ECUID1", fastecu::logging::LoggerSelection{});
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_THAT(status.error().detail, HasSubstr("read-only volume"));
}

} // namespace
