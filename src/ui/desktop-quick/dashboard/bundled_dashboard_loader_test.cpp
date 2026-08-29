#include "src/ui/desktop-quick/dashboard/bundled_dashboard_loader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <ranges>
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

constexpr std::string_view kSparklineDashboard = R"(<?xml version="1.0" encoding="utf-8"?>
<omnihaste-dashboard format-version="1">
    <metadata name="Sparkline fixture" description="Reject non-numeric cards"/>
    <connection protocol="cdbg" transport="raw-can" bitrate="500000" identifier-width="11" request-id="0x630" reply-id="0x631" stream-instance="0" sampling-interval-ms="50">
        <retry poll-timeout-ms="100" silence-threshold="3" reconnect-attempts="3" reconnect-period-ms="250"/>
    </connection>
    <channels>
        <channel id="CDBG_ENGINE_RPM" name="Engine RPM" description="engine_rpm uint16" address="0x804cfc" length="2" raw-assembly="unsigned-integer-decimal">
            <conversion id="default" expression="x*1000/256" unit="rpm" precision="0" gauge-min="0" gauge-max="8000" gauge-step="500"/>
        </channel>
    </channels>
    <cards>
        <card id="rpm-history" channel-id="CDBG_ENGINE_RPM" conversion-id="default" display-type="sparkline" order="0" sparkline-history-seconds="30"/>
    </cards>
</omnihaste-dashboard>
)";

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

    ASSERT_TRUE(document) << document.error().detail;
    EXPECT_EQ(document->metadata.name, "Colt CDBG Dashboard");
    EXPECT_TRUE(std::ranges::all_of(document->cards, [](const auto& card)
                                    { return card.display_type == dashboard::CardDisplayType::Numeric; }));
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

TEST(BundledDashboardLoaderTest, RejectsSparklineCardById)
{
    InMemoryFileRepository repository;
    repository.files.emplace(std::string(kBundledColtDashboardHandle), bytes_of(kSparklineDashboard));
    InMemoryAtomicFileWriter writer;
    dashboard::DashboardDocumentService service(repository, writer);

    const auto document = load_bundled_colt_dashboard(service);

    ASSERT_FALSE(document);
    EXPECT_EQ(document.error().kind, ErrorKind::Unsupported);
    EXPECT_TRUE(document.error().detail.starts_with("cards[rpm-history].display-type")) << document.error().detail;
}
