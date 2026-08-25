#include "src/backend/dashboard/dashboard_document_service.h"

#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/dashboard/test_fixtures.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

namespace
{

using fastecu::Error;
using fastecu::ErrorKind;
using fastecu::InMemoryAtomicFileWriter;
using fastecu::InMemoryFileRepository;
using fastecu::dashboard::CanIdentifierWidth;
using fastecu::dashboard::DashboardDocumentService;
using fastecu::dashboard::LegacyCdbgImportDefaults;
using fastecu::dashboard::RetryPolicy;
using ::testing::SizeIs;

constexpr std::string_view kCanonicalDocument = R"(<?xml version="1.0" encoding="utf-8"?>
<omnihaste-dashboard format-version="1">
    <metadata name="Colt Dashboard" description="Example"/>
    <connection protocol="cdbg" transport="raw-can" bitrate="500000" identifier-width="11" request-id="0x630" reply-id="0x631" stream-instance="0" sampling-interval-ms="50">
        <retry poll-timeout-ms="100" silence-threshold="3" reconnect-attempts="3" reconnect-period-ms="250"/>
    </connection>
    <channels>
        <channel id="CDBG_ENGINE_RPM" name="Engine RPM" description="engine_rpm uint16" address="0x804cfc" length="2" raw-assembly="unsigned-integer-decimal">
            <conversion id="conversion-1" expression="x*1000/256" unit="rpm" precision="0" gauge-min="0" gauge-max="8000" gauge-step="500"/>
        </channel>
    </channels>
    <cards/>
</omnihaste-dashboard>
)";

constexpr std::string_view kLegacyCatalog = R"(<logger><protocols><protocol id="CDBG"><parameters>
  <parameter id="P1" name="First" desc="First description" length="2" enabled="1">
    <address>0x1234</address><conversions>
      <conversion units="rpm" expr="x*2" format="0" gauge_min="0" gauge_max="8000" gauge_step="500"/>
    </conversions>
  </parameter>
</parameters></protocol></protocols></logger>)";

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return {text.begin(), text.end()};
}

LegacyCdbgImportDefaults valid_defaults()
{
    return LegacyCdbgImportDefaults{
        .document_name = "Imported Colt",
        .bitrate = 500000,
        .identifier_width = CanIdentifierWidth::Standard,
        .stream_instance = 2,
        .sampling_interval_ms = 25,
        .retry = RetryPolicy{120, 4, 5, 300},
    };
}

class DashboardDocumentServiceTest : public ::testing::Test
{
  protected:
    DashboardDocumentService service()
    {
        return DashboardDocumentService(repository_, writer_);
    }

    InMemoryFileRepository repository_;
    InMemoryAtomicFileWriter writer_;
};

TEST_F(DashboardDocumentServiceTest, LoadReadsOnceAndReturnsTheDecodedDocument)
{
    repository_.files["dashboard.ohd"] = bytes_of(kCanonicalDocument);

    const auto document = service().load("dashboard.ohd");

    ASSERT_TRUE(document) << document.error().detail;
    EXPECT_EQ(*document, fastecu::dashboard::test::valid_document());
    EXPECT_EQ(repository_.read_count("dashboard.ohd"), 1);
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(DashboardDocumentServiceTest, LoadRejectsMalformedAndInvalidDocumentsWithoutReturningAModel)
{
    repository_.files["malformed.ohd"] = bytes_of("<omnihaste-dashboard");
    repository_.files["invalid.ohd"] = bytes_of("<dashboard format-version=\"1\"/>");

    const auto malformed = service().load("malformed.ohd");
    const auto invalid = service().load("invalid.ohd");

    EXPECT_FALSE(malformed);
    EXPECT_EQ(malformed.error().kind, ErrorKind::InvalidConfig);
    EXPECT_FALSE(invalid);
    EXPECT_EQ(invalid.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(DashboardDocumentServiceTest, LoadPassesThroughUnsupportedVersionUnchanged)
{
    repository_.files["future.ohd"] = bytes_of("<omnihaste-dashboard format-version=\"2\"/>");

    const auto document = service().load("future.ohd");

    ASSERT_FALSE(document);
    EXPECT_EQ(document.error(), (Error{ErrorKind::Unsupported, "metadata.format-version: unsupported version 2"}));
}

TEST_F(DashboardDocumentServiceTest, LoadPassesThroughRepositoryErrorsUnchanged)
{
    const Error expected{ErrorKind::Timeout, "repository timed out"};
    repository_.next_read_result = std::unexpected(expected);

    const auto document = service().load("dashboard.ohd");

    ASSERT_FALSE(document);
    EXPECT_EQ(document.error(), expected);
    EXPECT_EQ(repository_.read_count("dashboard.ohd"), 1);
}

TEST_F(DashboardDocumentServiceTest, SaveWritesCanonicalBytesInOneAtomicReplacement)
{
    const auto status = service().save("dashboard.ohd", fastecu::dashboard::test::valid_document());

    ASSERT_TRUE(status) << status.error().detail;
    ASSERT_THAT(writer_.replace_calls, SizeIs(1));
    EXPECT_EQ(writer_.replace_calls.front().handle, "dashboard.ohd");
    EXPECT_EQ(writer_.replace_calls.front().data, bytes_of(kCanonicalDocument));
    EXPECT_EQ(writer_.files.at("dashboard.ohd"), bytes_of(kCanonicalDocument));
}

TEST_F(DashboardDocumentServiceTest, SaveRejectsInvalidDocumentsBeforeReplacingAnything)
{
    auto document = fastecu::dashboard::test::valid_document();
    document.metadata.format_version = 2;

    const auto status = service().save("dashboard.ohd", document);

    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(writer_.replace_calls.empty());
    EXPECT_TRUE(writer_.files.empty());
}

TEST_F(DashboardDocumentServiceTest, SavePassesThroughReplacementErrorsWithoutUpdatingDestination)
{
    writer_.files["dashboard.ohd"] = bytes_of("previous contents");
    const Error expected{ErrorKind::Internal, "disk full"};
    writer_.replace_error = expected;

    const auto status = service().save("dashboard.ohd", fastecu::dashboard::test::valid_document());

    ASSERT_FALSE(status);
    EXPECT_EQ(status.error(), expected);
    ASSERT_THAT(writer_.replace_calls, SizeIs(1));
    EXPECT_EQ(writer_.files.at("dashboard.ohd"), bytes_of("previous contents"));
}

TEST_F(DashboardDocumentServiceTest, ImportReadsOnceAndReturnsAnUnsavedDocument)
{
    repository_.files["logger.xml"] = bytes_of(kLegacyCatalog);

    const auto document = service().import_legacy_cdbg_catalog("logger.xml", valid_defaults());

    ASSERT_TRUE(document) << document.error().detail;
    EXPECT_EQ(document->metadata.name, "Imported Colt");
    EXPECT_EQ(document->connection.stream_instance, 2);
    ASSERT_THAT(document->channels, SizeIs(1));
    EXPECT_EQ(document->channels.front().id, "P1");
    EXPECT_EQ(repository_.read_count("logger.xml"), 1);
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(DashboardDocumentServiceTest, ImportParseAndValidationFailuresDoNotWrite)
{
    repository_.files["malformed.xml"] = bytes_of("<logger><protocols>");
    repository_.files["invalid.xml"] = bytes_of(kLegacyCatalog);
    auto invalid_defaults = valid_defaults();
    invalid_defaults.document_name.clear();

    const auto malformed = service().import_legacy_cdbg_catalog("malformed.xml", valid_defaults());
    const auto invalid = service().import_legacy_cdbg_catalog("invalid.xml", invalid_defaults);

    EXPECT_FALSE(malformed);
    EXPECT_EQ(malformed.error().kind, ErrorKind::InvalidConfig);
    EXPECT_FALSE(invalid);
    EXPECT_EQ(invalid.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(DashboardDocumentServiceTest, ImportReadFailuresDoNotWrite)
{
    const Error expected{ErrorKind::Disconnected, "catalog unavailable"};
    repository_.next_read_result = std::unexpected(expected);

    const auto document = service().import_legacy_cdbg_catalog("logger.xml", valid_defaults());

    ASSERT_FALSE(document);
    EXPECT_EQ(document.error(), expected);
    EXPECT_EQ(repository_.read_count("logger.xml"), 1);
    EXPECT_TRUE(writer_.replace_calls.empty());
}

} // namespace
