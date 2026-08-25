#include "src/backend/dashboard/dashboard_validation.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

#include "src/backend/dashboard/test_fixtures.h"

namespace
{
using namespace fastecu;
using namespace fastecu::dashboard;

void expect_invalid(const DashboardDocument& document, std::string_view path)
{
    const auto result = validate_dashboard_document(document);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(result.error().detail.starts_with(path)) << result.error().detail;
}

DashboardCard card(std::string id, std::string channel_id, std::string conversion_id, std::uint32_t order = 0)
{
    return DashboardCard{
        .id = std::move(id),
        .channel_id = std::move(channel_id),
        .conversion_id = std::move(conversion_id),
        .display_type = CardDisplayType::Numeric,
        .title = std::nullopt,
        .order = order,
        .gauge_bounds = std::nullopt,
        .sparkline_history_seconds = std::nullopt,
    };
}

DashboardDocument document_with_two_channels()
{
    auto document = test::valid_document();
    auto second = document.channels.front();
    second.id = "CDBG_COOLANT_TEMP";
    second.name = "Coolant Temperature";
    second.description = "coolant_temp uint16";
    second.address = 0x804d00;
    second.conversions.front().id = "conversion-2";
    second.conversions.front().unit = "C";
    document.channels.push_back(std::move(second));
    return document;
}
} // namespace

TEST(DashboardValidation, AcceptsCanonicalDocumentWithEmptyCardsAndNoPreferredAdapter)
{
    EXPECT_TRUE(validate_dashboard_document(test::valid_document()));
}

TEST(DashboardValidation, ReturnsTheFirstErrorInDocumentOrder)
{
    auto document = test::valid_document();
    document.metadata.format_version = 2;
    document.metadata.name.clear();
    document.connection.bitrate = 0;
    expect_invalid(document, "metadata.format-version");
}

TEST(DashboardValidation, RejectsUnsupportedMetadata)
{
    auto document = test::valid_document();
    document.metadata.format_version = 2;
    expect_invalid(document, "metadata.format-version");

    document = test::valid_document();
    document.metadata.name.clear();
    expect_invalid(document, "metadata.name");
}

TEST(DashboardValidation, RejectsUnsupportedConnectionEnums)
{
    auto document = test::valid_document();
    document.connection.protocol = static_cast<DashboardProtocol>(99);
    expect_invalid(document, "connection.protocol");

    document = test::valid_document();
    document.connection.transport = static_cast<DashboardTransport>(99);
    expect_invalid(document, "connection.transport");

    document = test::valid_document();
    document.connection.identifier_width = static_cast<CanIdentifierWidth>(99);
    expect_invalid(document, "connection.identifier-width");
}

TEST(DashboardValidation, RejectsZeroOperationalConnectionValues)
{
    struct Case
    {
        void (*mutate)(DashboardDocument&);
        std::string_view path;
    };
    constexpr std::array cases{
        Case{[](DashboardDocument& document) { document.connection.bitrate = 0; }, "connection.bitrate"},
        Case{[](DashboardDocument& document) { document.connection.sampling_interval_ms = 0; },
             "connection.sampling-interval-ms"},
        Case{[](DashboardDocument& document) { document.connection.retry.poll_timeout_ms = 0; },
             "connection.retry.poll-timeout-ms"},
        Case{[](DashboardDocument& document) { document.connection.retry.silence_threshold = 0; },
             "connection.retry.silence-threshold"},
        Case{[](DashboardDocument& document) { document.connection.retry.reconnect_attempts = 0; },
             "connection.retry.reconnect-attempts"},
        Case{[](DashboardDocument& document) { document.connection.retry.reconnect_period_ms = 0; },
             "connection.retry.reconnect-period-ms"},
    };

    for (const auto& test_case : cases)
    {
        auto document = test::valid_document();
        test_case.mutate(document);
        expect_invalid(document, test_case.path);
    }
}

TEST(DashboardValidation, AcceptsInclusiveCanIdentifierMaxima)
{
    auto standard = test::valid_document();
    standard.connection.request_id = 0x7ff;
    standard.connection.reply_id = 0x7fe;
    EXPECT_TRUE(validate_dashboard_document(standard));

    auto extended = test::valid_document();
    extended.connection.identifier_width = CanIdentifierWidth::Extended;
    extended.connection.request_id = 0x1fffffff;
    extended.connection.reply_id = 0x1ffffffe;
    EXPECT_TRUE(validate_dashboard_document(extended));
}

TEST(DashboardValidation, RejectsCanIdentifiersAboveTheSelectedWidth)
{
    auto standard = test::valid_document();
    standard.connection.request_id = 0x800;
    expect_invalid(standard, "connection.request-id");

    standard = test::valid_document();
    standard.connection.reply_id = 0x800;
    expect_invalid(standard, "connection.reply-id");

    auto extended = test::valid_document();
    extended.connection.identifier_width = CanIdentifierWidth::Extended;
    extended.connection.request_id = 0x20000000;
    expect_invalid(extended, "connection.request-id");

    extended = test::valid_document();
    extended.connection.identifier_width = CanIdentifierWidth::Extended;
    extended.connection.reply_id = 0x20000000;
    expect_invalid(extended, "connection.reply-id");
}

TEST(DashboardValidation, RejectsDuplicateRequestAndReplyIdentifiers)
{
    auto document = test::valid_document();
    document.connection.reply_id = document.connection.request_id;
    expect_invalid(document, "connection.reply-id");
}

TEST(DashboardValidation, AcceptsBothPortableAdapterFamilies)
{
    for (const AdapterKind kind : {AdapterKind::J2534, AdapterKind::SocketCan})
    {
        auto document = test::valid_document();
        document.connection.preferred_adapter = PreferredAdapter{
            .kind = kind,
            .vendor = "OpenECU",
            .display_name = "Portable CAN",
        };
        EXPECT_TRUE(validate_dashboard_document(document));
    }
}

TEST(DashboardValidation, RejectsInvalidPreferredAdapters)
{
    auto document = test::valid_document();
    document.connection.preferred_adapter = PreferredAdapter{
        .kind = static_cast<AdapterKind>(99),
        .vendor = "OpenECU",
        .display_name = "Portable CAN",
    };
    expect_invalid(document, "connection.preferred-adapter.kind");

    document = test::valid_document();
    document.connection.preferred_adapter = PreferredAdapter{AdapterKind::J2534, "", "Portable CAN"};
    expect_invalid(document, "connection.preferred-adapter.vendor");

    document = test::valid_document();
    document.connection.preferred_adapter = PreferredAdapter{AdapterKind::SocketCan, "Linux", ""};
    expect_invalid(document, "connection.preferred-adapter.display-name");
}

TEST(DashboardValidation, RejectsEmptyAndDuplicateChannelIds)
{
    auto document = test::valid_document();
    document.channels.front().id.clear();
    expect_invalid(document, "channels[0].id");

    document = test::valid_document();
    document.channels.push_back(document.channels.front());
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].id");
}

TEST(DashboardValidation, RejectsEmptyRequiredChannelName)
{
    auto document = test::valid_document();
    document.channels.front().name.clear();
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].name");
}

TEST(DashboardValidation, RejectsEmptyRequiredChannelDescription)
{
    auto document = test::valid_document();
    document.channels.front().description.clear();
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].description");
}

TEST(DashboardValidation, RejectsUnsupportedChannelLengths)
{
    for (const std::uint8_t length : {0, 3, 8})
    {
        auto document = test::valid_document();
        document.channels.front().length = length;
        expect_invalid(document, "channels[CDBG_ENGINE_RPM].length");
    }
}

TEST(DashboardValidation, AcceptsOneByteChannels)
{
    auto document = test::valid_document();
    document.channels.front().length = 1;
    EXPECT_TRUE(validate_dashboard_document(document));
}

TEST(DashboardValidation, AcceptsFourByteChannels)
{
    auto document = test::valid_document();
    document.channels.front().length = 4;
    EXPECT_TRUE(validate_dashboard_document(document));
}

TEST(DashboardValidation, RejectsUnsupportedRawAssembly)
{
    auto document = test::valid_document();
    document.channels.front().raw_assembly = static_cast<RawAssembly>(99);
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].raw-assembly");
}

TEST(DashboardValidation, RejectsMissingAndDuplicateConversions)
{
    auto document = test::valid_document();
    document.channels.front().conversions.clear();
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].conversions");

    document = test::valid_document();
    document.channels.front().conversions.front().id.clear();
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].conversions[0].id");

    document = test::valid_document();
    document.channels.front().conversions.push_back(document.channels.front().conversions.front());
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].conversions[conversion-1].id");
}

TEST(DashboardValidation, RejectsEmptyRequiredConversionUnit)
{
    auto document = test::valid_document();
    document.channels.front().conversions.front().unit.clear();
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].conversions[conversion-1].unit");
}

TEST(DashboardValidation, RejectsInvalidConversionSemantics)
{
    auto document = test::valid_document();
    document.channels.front().conversions.front().expression = "x*/2";
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].conversions[conversion-1].expression");

    document = test::valid_document();
    document.channels.front().conversions.front().expression = "1/(x-x)";
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].conversions[conversion-1].expression");

    document = test::valid_document();
    document.channels.front().conversions.front().precision = 16;
    expect_invalid(document, "channels[CDBG_ENGINE_RPM].conversions[conversion-1].precision");
}

TEST(DashboardValidation, RejectsInvalidConversionGaugeBounds)
{
    struct Bounds
    {
        double minimum;
        double maximum;
        double step;
    };
    const std::array cases{
        Bounds{std::numeric_limits<double>::quiet_NaN(), 8000.0, 500.0},
        Bounds{0.0, std::numeric_limits<double>::infinity(), 500.0},
        Bounds{0.0, 8000.0, -std::numeric_limits<double>::infinity()},
        Bounds{8000.0, 8000.0, 500.0},
        Bounds{9000.0, 8000.0, 500.0},
        Bounds{0.0, 8000.0, 0.0},
        Bounds{0.0, 8000.0, -1.0},
    };

    for (const Bounds bounds : cases)
    {
        auto document = test::valid_document();
        auto& conversion = document.channels.front().conversions.front();
        conversion.gauge_min = bounds.minimum;
        conversion.gauge_max = bounds.maximum;
        conversion.gauge_step = bounds.step;
        expect_invalid(document, "channels[CDBG_ENGINE_RPM].conversions[conversion-1].gauge");
    }
}

TEST(DashboardValidation, RejectsEmptyAndDuplicateCardIds)
{
    auto document = test::valid_document();
    document.cards.push_back(card("", "CDBG_ENGINE_RPM", "conversion-1"));
    expect_invalid(document, "cards[0].id");

    document = document_with_two_channels();
    document.cards = {
        card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1", 0),
        card("rpm-card", "CDBG_COOLANT_TEMP", "conversion-2", 1),
    };
    expect_invalid(document, "cards[rpm-card].id");
}

TEST(DashboardValidation, RejectsDuplicateAndNoncontiguousCardOrders)
{
    auto document = document_with_two_channels();
    document.cards = {
        card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1", 0),
        card("coolant-card", "CDBG_COOLANT_TEMP", "conversion-2", 0),
    };
    expect_invalid(document, "cards[coolant-card].order");

    document = document_with_two_channels();
    document.cards = {
        card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1", 0),
        card("coolant-card", "CDBG_COOLANT_TEMP", "conversion-2", 2),
    };
    expect_invalid(document, "cards[coolant-card].order");
}

TEST(DashboardValidation, RejectsTwoCardsForOneChannel)
{
    auto document = test::valid_document();
    document.cards = {
        card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1", 0),
        card("rpm-card-2", "CDBG_ENGINE_RPM", "conversion-1", 1),
    };
    expect_invalid(document, "cards[rpm-card-2].channel-id");
}

TEST(DashboardValidation, RejectsMissingCardReferences)
{
    auto document = test::valid_document();
    document.cards.push_back(card("rpm-card", "missing", "conversion-1"));
    expect_invalid(document, "cards[rpm-card].channel-id");

    document = test::valid_document();
    document.cards.push_back(card("rpm-card", "CDBG_ENGINE_RPM", "missing"));
    expect_invalid(document, "cards[rpm-card].conversion-id");
}

TEST(DashboardValidation, RejectsInvalidCardDisplayType)
{
    auto document = test::valid_document();
    auto invalid = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
    invalid.display_type = static_cast<CardDisplayType>(99);
    document.cards.push_back(std::move(invalid));
    expect_invalid(document, "cards[rpm-card].display-type");
}

TEST(DashboardValidation, RejectsNumericCardSpecificFields)
{
    auto document = test::valid_document();
    auto numeric = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
    numeric.gauge_bounds = GaugeBoundsOverride{0.0, 8000.0, 500.0};
    document.cards.push_back(std::move(numeric));
    expect_invalid(document, "cards[rpm-card].gauge");

    document = test::valid_document();
    numeric = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
    numeric.sparkline_history_seconds = 60;
    document.cards.push_back(std::move(numeric));
    expect_invalid(document, "cards[rpm-card].sparkline-history-seconds");
}

TEST(DashboardValidation, AcceptsSparklineHistoryInclusiveBounds)
{
    for (const std::uint16_t seconds : {1, 300})
    {
        auto document = test::valid_document();
        auto sparkline = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
        sparkline.display_type = CardDisplayType::Sparkline;
        sparkline.sparkline_history_seconds = seconds;
        document.cards.push_back(std::move(sparkline));
        EXPECT_TRUE(validate_dashboard_document(document));
    }
}

TEST(DashboardValidation, RejectsInvalidSparklineFields)
{
    auto document = test::valid_document();
    auto sparkline = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
    sparkline.display_type = CardDisplayType::Sparkline;
    document.cards.push_back(std::move(sparkline));
    expect_invalid(document, "cards[rpm-card].sparkline-history-seconds");

    document = test::valid_document();
    sparkline = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
    sparkline.display_type = CardDisplayType::Sparkline;
    sparkline.gauge_bounds = GaugeBoundsOverride{0.0, 8000.0, 500.0};
    sparkline.sparkline_history_seconds = 60;
    document.cards.push_back(std::move(sparkline));
    expect_invalid(document, "cards[rpm-card].gauge");

    for (const std::uint16_t seconds : {0, 301})
    {
        document = test::valid_document();
        sparkline = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
        sparkline.display_type = CardDisplayType::Sparkline;
        sparkline.sparkline_history_seconds = seconds;
        document.cards.push_back(std::move(sparkline));
        expect_invalid(document, "cards[rpm-card].sparkline-history-seconds");
    }
}

TEST(DashboardValidation, AcceptsAHorizontalGaugeWithValidOverrideBounds)
{
    auto document = test::valid_document();
    auto gauge = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
    gauge.display_type = CardDisplayType::HorizontalGauge;
    gauge.gauge_bounds = GaugeBoundsOverride{0.0, 8000.0, 500.0};
    document.cards.push_back(std::move(gauge));
    EXPECT_TRUE(validate_dashboard_document(document));
}

TEST(DashboardValidation, RejectsInvalidHorizontalGaugeFields)
{
    auto document = test::valid_document();
    auto gauge = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
    gauge.display_type = CardDisplayType::HorizontalGauge;
    document.cards.push_back(std::move(gauge));
    expect_invalid(document, "cards[rpm-card].gauge");

    struct Bounds
    {
        double minimum;
        double maximum;
        double step;
    };
    const std::array cases{
        Bounds{std::numeric_limits<double>::quiet_NaN(), 8000.0, 500.0},
        Bounds{0.0, std::numeric_limits<double>::infinity(), 500.0},
        Bounds{0.0, 8000.0, -std::numeric_limits<double>::infinity()},
        Bounds{8000.0, 8000.0, 500.0},
        Bounds{9000.0, 8000.0, 500.0},
        Bounds{0.0, 8000.0, 0.0},
        Bounds{0.0, 8000.0, -1.0},
    };
    for (const Bounds bounds : cases)
    {
        document = test::valid_document();
        gauge = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
        gauge.display_type = CardDisplayType::HorizontalGauge;
        gauge.gauge_bounds = GaugeBoundsOverride{bounds.minimum, bounds.maximum, bounds.step};
        document.cards.push_back(std::move(gauge));
        expect_invalid(document, "cards[rpm-card].gauge");
    }

    document = test::valid_document();
    gauge = card("rpm-card", "CDBG_ENGINE_RPM", "conversion-1");
    gauge.display_type = CardDisplayType::HorizontalGauge;
    gauge.gauge_bounds = GaugeBoundsOverride{0.0, 8000.0, 500.0};
    gauge.sparkline_history_seconds = 60;
    document.cards.push_back(std::move(gauge));
    expect_invalid(document, "cards[rpm-card].sparkline-history-seconds");
}
