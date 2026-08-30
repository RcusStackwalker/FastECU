#include "src/ui/desktop-quick/dashboard/bundled_dashboard_loader.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "src/backend/dashboard/dashboard_document_service.h"
#include "src/backend/dashboard/dashboard_session_builder.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"

namespace
{
using namespace fastecu;
using namespace fastecu::desktop_quick;

constexpr std::string_view kMixedTypeDashboard = R"(<?xml version="1.0" encoding="utf-8"?>
<omnihaste-dashboard format-version="1">
    <metadata name="Mixed display fixture" description="Accept all portable-v1 display types"/>
    <connection protocol="cdbg" transport="raw-can" bitrate="500000" identifier-width="11" request-id="0x630" reply-id="0x631" stream-instance="0" sampling-interval-ms="50">
        <retry poll-timeout-ms="100" silence-threshold="3" reconnect-attempts="3" reconnect-period-ms="250"/>
    </connection>
    <channels>
        <channel id="CDBG_ENGINE_RPM" name="Engine RPM" description="engine_rpm uint16" address="0x804cfc" length="2" raw-assembly="unsigned-integer-decimal">
            <conversion id="default" expression="x*1000/256" unit="rpm" precision="0" gauge-min="0" gauge-max="8000" gauge-step="500"/>
        </channel>
        <channel id="CDBG_KNOCK_SUM" name="Knock Sum" description="knock_sum raw uint16" address="0x804fbe" length="2" raw-assembly="unsigned-integer-decimal">
            <conversion id="default" expression="x" unit="raw" precision="0" gauge-min="0" gauge-max="65535" gauge-step="1"/>
        </channel>
        <channel id="CDBG_ECU_LOAD" name="ECU Load" description="ecu_load uint16" address="0x804d2a" length="2" raw-assembly="unsigned-integer-decimal">
            <conversion id="default" expression="x*10/32" unit="%" precision="1" gauge-min="0" gauge-max="300" gauge-step="10"/>
        </channel>
    </channels>
    <cards>
        <card id="engine-rpm" channel-id="CDBG_ENGINE_RPM" conversion-id="default" display-type="numeric" order="0"/>
        <card id="knock-sum" channel-id="CDBG_KNOCK_SUM" conversion-id="default" display-type="sparkline" order="1" sparkline-history-seconds="30"/>
        <card id="ecu-load" channel-id="CDBG_ECU_LOAD" conversion-id="default" display-type="horizontal-gauge" order="2" gauge-min="0" gauge-max="100" gauge-step="10"/>
    </cards>
</omnihaste-dashboard>
)";

constexpr std::string_view kPortableInvalidDashboard = R"(<dashboard format-version="1"/>)";

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t *>(text.data()),
            reinterpret_cast<const std::uint8_t *>(text.data()) + text.size()};
}
} // namespace

TEST(BundledDashboardLoaderTest, LoadsRealResourceAndBuildsSession)
{
    QtFileRepository repository;
    InMemoryAtomicFileWriter writer;
    dashboard::DashboardDocumentService service(repository, writer);

    auto document = load_bundled_colt_dashboard(service);

    ASSERT_TRUE(document.has_value()) << document.error().detail;
    EXPECT_EQ(document->metadata.name, "Colt CDBG Dashboard");
    ASSERT_EQ(document->cards.size(), 4U);
    EXPECT_EQ(document->cards[0].display_type, dashboard::CardDisplayType::Numeric);
    EXPECT_EQ(document->cards[1].display_type, dashboard::CardDisplayType::HorizontalGauge);
    EXPECT_EQ(document->cards[2].display_type, dashboard::CardDisplayType::Sparkline);
    EXPECT_EQ(document->cards[3].display_type, dashboard::CardDisplayType::Numeric);
    EXPECT_TRUE(dashboard::prepare_dashboard_session(*document));
}

TEST(BundledDashboardLoaderTest, PropagatesMissingResourceErrorUnchanged)
{
    InMemoryFileRepository repository;
    InMemoryAtomicFileWriter writer;
    dashboard::DashboardDocumentService service(repository, writer);

    const auto document = load_bundled_colt_dashboard(service);

    ASSERT_FALSE(document);
    EXPECT_EQ(document.error(), (Error{ErrorKind::InvalidConfig, "no such handle"}));
    ASSERT_EQ(repository.read_handles.size(), 1U);
    EXPECT_EQ(repository.read_handles.front(), kBundledColtDashboardHandle);
}

TEST(BundledDashboardLoaderTest, LoadsPortableMixedTypeCardsInDocumentOrder)
{
    InMemoryFileRepository repository;
    repository.files.emplace(std::string(kBundledColtDashboardHandle), bytes_of(kMixedTypeDashboard));
    InMemoryAtomicFileWriter writer;
    dashboard::DashboardDocumentService service(repository, writer);

    const auto document = load_bundled_colt_dashboard(service);

    ASSERT_TRUE(document.has_value()) << document.error().detail;
    ASSERT_EQ(document->cards.size(), 3U);
    EXPECT_EQ(document->cards[0].display_type, dashboard::CardDisplayType::Numeric);
    EXPECT_EQ(document->cards[1].display_type, dashboard::CardDisplayType::Sparkline);
    EXPECT_EQ(document->cards[2].display_type, dashboard::CardDisplayType::HorizontalGauge);
}

TEST(BundledDashboardLoaderTest, PropagatesPortableInvalidDocumentErrorUnchanged)
{
    InMemoryFileRepository repository;
    repository.files.emplace(std::string(kBundledColtDashboardHandle), bytes_of(kPortableInvalidDashboard));
    InMemoryAtomicFileWriter writer;
    dashboard::DashboardDocumentService service(repository, writer);

    const auto document = load_bundled_colt_dashboard(service);

    ASSERT_FALSE(document);
    EXPECT_EQ(document.error(), (Error{ErrorKind::InvalidConfig, "document.root: expected <omnihaste-dashboard>"}));
}
