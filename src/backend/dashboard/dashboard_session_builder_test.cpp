#include "src/backend/dashboard/dashboard_session_builder.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "src/backend/dashboard/test_fixtures.h"
#include "src/backend/logging/logging_types.h"

namespace fastecu::dashboard
{
namespace
{
static_assert(!std::is_copy_constructible_v<PreparedDashboardSession>);
static_assert(!std::is_copy_assignable_v<PreparedDashboardSession>);
static_assert(std::is_move_constructible_v<PreparedDashboardSession>);
static_assert(std::is_move_assignable_v<PreparedDashboardSession>);

DashboardCard numeric_card(std::string id, std::string channel_id, std::string conversion_id, std::uint32_t order)
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

DashboardChannel coolant_channel()
{
    return DashboardChannel{
        .id = "CDBG_COOLANT_TEMP",
        .name = "Coolant Temperature",
        .description = "coolant_temperature uint8",
        .address = 0x804d10,
        .length = 1,
        .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
        .conversions =
            {
                DashboardConversion{
                    .id = "fahrenheit",
                    .expression = "x*9/5-40",
                    .unit = "deg F",
                    .precision = 1,
                    .gauge_min = -40.0,
                    .gauge_max = 260.0,
                    .gauge_step = 10.0,
                },
                DashboardConversion{
                    .id = "celsius",
                    .expression = "x-40",
                    .unit = "deg C",
                    .precision = 0,
                    .gauge_min = -40.0,
                    .gauge_max = 130.0,
                    .gauge_step = 10.0,
                },
            },
    };
}

DashboardDocument selected_document()
{
    auto document = test::valid_document();
    document.cards = {
        numeric_card("rpm", document.channels[0].id, document.channels[0].conversions[0].id, 0),
    };
    return document;
}

void expect_invalid(const DashboardDocument& document, std::string_view detail)
{
    const auto result = prepare_dashboard_session(document);

    EXPECT_FALSE(result.has_value());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(result.error().detail, detail);
}

TEST(DashboardSessionBuilderTest, PreparesSelectedChannelsInCatalogOrderWithChosenConversionsAndPolicy)
{
    auto document = test::valid_document();
    document.channels.push_back(coolant_channel());
    document.cards = {
        numeric_card("coolant", document.channels[1].id, "celsius", 0),
        numeric_card("rpm", document.channels[0].id, document.channels[0].conversions[0].id, 1),
    };

    const DashboardConversion& chosen_conversion = document.channels[0].conversions[0];
    const auto result = prepare_dashboard_session(document);

    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->session().protocol(), logging::LoggingProtocolId::Cdbg);
    ASSERT_EQ(result->session().channels().size(), 2U);
    EXPECT_EQ(result->session().channels()[0].id, document.channels[0].id);
    EXPECT_EQ(result->session().channels()[1].id, document.channels[1].id);
    EXPECT_EQ(result->session().channels()[0].from_byte_expression, chosen_conversion.expression);
    EXPECT_EQ(result->session().channels()[0].address, document.channels[0].address);
    EXPECT_EQ(result->session().channels()[0].length, document.channels[0].length);
    EXPECT_EQ(result->session().channels()[0].raw_assembly, logging::RawAssembly::UnsignedIntegerDecimal);
    EXPECT_EQ(result->session().channels()[0].unit, chosen_conversion.unit);
    EXPECT_EQ(result->session().channels()[0].decimal_precision, chosen_conversion.precision);
    EXPECT_EQ(result->session().channels()[1].from_byte_expression, "x-40");
    EXPECT_EQ(result->session().channels()[1].unit, "deg C");
    EXPECT_EQ(result->session().channels()[1].decimal_precision, 0);

    EXPECT_EQ(result->config().request_id(), document.connection.request_id);
    EXPECT_EQ(result->config().reply_id(), document.connection.reply_id);
    EXPECT_EQ(result->config().stream_instance(), document.connection.stream_instance);
    EXPECT_EQ(result->config().sampling_interval_ms(), document.connection.sampling_interval_ms);

    const logging::LoggingPolicy& policy = result->session().policy();
    EXPECT_EQ(policy.poll_timeout_ms, document.connection.retry.poll_timeout_ms);
    EXPECT_EQ(policy.car_silence_miss_threshold, document.connection.retry.silence_threshold);
    EXPECT_EQ(policy.reconnect_initial_delay_ms, document.connection.retry.reconnect_period_ms);
    EXPECT_EQ(policy.reconnect_period_ms, document.connection.retry.reconnect_period_ms);
    EXPECT_EQ(policy.max_reconnect_attempts, document.connection.retry.reconnect_attempts);
}

TEST(DashboardSessionBuilderTest, OmitsUnusedCatalogChannelsWithoutChangingSelectedCatalogOrder)
{
    auto document = test::valid_document();
    DashboardChannel unused = coolant_channel();
    unused.id = "UNUSED_MIDDLE";
    unused.address = 0x804d20;
    DashboardChannel selected_last = coolant_channel();
    selected_last.id = "SELECTED_LAST";
    selected_last.address = 0x804d21;
    document.channels.push_back(std::move(unused));
    document.channels.push_back(std::move(selected_last));
    document.cards = {
        numeric_card("last", document.channels[2].id, "fahrenheit", 0),
        numeric_card("first", document.channels[0].id, document.channels[0].conversions[0].id, 1),
    };

    const auto result = prepare_dashboard_session(document);

    ASSERT_TRUE(result) << result.error().detail;
    ASSERT_EQ(result->session().channels().size(), 2U);
    EXPECT_EQ(result->session().channels()[0].id, document.channels[0].id);
    EXPECT_EQ(result->session().channels()[1].id, document.channels[2].id);
}

TEST(DashboardSessionBuilderTest, MovesThePreparedSessionAndConfigOutTogether)
{
    auto document = selected_document();
    auto result = prepare_dashboard_session(document);
    ASSERT_TRUE(result) << result.error().detail;

    auto [session, config] = std::move(*result).into_parts();

    EXPECT_EQ(session.channels()[0].id, "CDBG_ENGINE_RPM");
    EXPECT_EQ(config.request_id(), 0x630U);
}

TEST(DashboardSessionBuilderTest, RejectsASelectionWithNoCards)
{
    expect_invalid(test::valid_document(), "cards: no CDBG log parameters selected");
}

TEST(DashboardSessionBuilderTest, RejectsACardWhoseChannelReferenceIsMissing)
{
    auto document = selected_document();
    document.cards[0].channel_id = "missing";

    expect_invalid(document, "cards[rpm].channel-id: does not reference a channel");
}

TEST(DashboardSessionBuilderTest, RejectsACardWhoseConversionReferenceIsMissing)
{
    auto document = selected_document();
    document.cards[0].conversion_id = "missing";

    expect_invalid(document, "cards[rpm].conversion-id: does not reference a conversion on the channel");
}

TEST(DashboardSessionBuilderTest, RejectsMalformedSelectedConversionExpressionWithValidationPath)
{
    auto document = selected_document();
    document.channels[0].conversions[0].expression = "x/(";

    const auto result = prepare_dashboard_session(document);

    EXPECT_FALSE(result.has_value());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(result.error().detail,
              "channels[CDBG_ENGINE_RPM].conversions[conversion-1].expression: is not a valid conversion expression");
}

TEST(DashboardSessionBuilderTest, RejectsDuplicateCardIdentifiers)
{
    auto document = selected_document();
    document.channels.push_back(coolant_channel());
    document.cards.push_back(numeric_card("rpm", document.channels[1].id, "celsius", 1));

    expect_invalid(document, "cards[rpm].id: must be unique");
}

TEST(DashboardSessionBuilderTest, RejectsDuplicateCardOrder)
{
    auto document = selected_document();
    document.channels.push_back(coolant_channel());
    document.cards.push_back(numeric_card("coolant", document.channels[1].id, "celsius", 0));

    expect_invalid(document, "cards[coolant].order: orders must be the unique contiguous range starting at zero");
}

TEST(DashboardSessionBuilderTest, RejectsOneChannelSelectedByTwoCards)
{
    auto document = selected_document();
    document.cards.push_back(
        numeric_card("rpm-copy", document.channels[0].id, document.channels[0].conversions[0].id, 1));

    expect_invalid(document, "cards[rpm-copy].channel-id: only one card may reference a channel");
}

TEST(DashboardSessionBuilderTest, RejectsUnsupportedRawAssembly)
{
    auto document = selected_document();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- exercises invalid-value validation
    document.channels[0].raw_assembly = static_cast<RawAssembly>(99);

    expect_invalid(document, "channels[CDBG_ENGINE_RPM].raw-assembly: must be unsigned-integer-decimal");
}

TEST(DashboardSessionBuilderTest, RejectsRetryValuesThatDoNotFitTheGenericPolicy)
{
    struct TestCase
    {
        std::uint32_t RetryPolicy::*field;
        std::string_view path;
    };
    constexpr std::array<TestCase, 3> cases{
        TestCase{&RetryPolicy::poll_timeout_ms, "connection.retry.poll-timeout-ms"},
        TestCase{&RetryPolicy::silence_threshold, "connection.retry.silence-threshold"},
        TestCase{&RetryPolicy::reconnect_period_ms, "connection.retry.reconnect-period-ms"},
    };
    for (const TestCase& test_case : cases)
    {
        auto document = selected_document();
        document.connection.retry.*test_case.field = static_cast<std::uint32_t>(std::numeric_limits<int>::max()) + 1U;

        expect_invalid(document, std::string(test_case.path) + ": exceeds the generic logging policy integer range");
    }
}

TEST(DashboardSessionBuilderTest, RejectsInvalidCdbgSamplingInterval)
{
    auto document = selected_document();
    document.connection.sampling_interval_ms = 65536;

    expect_invalid(document, "connection.sampling-interval-ms: is not encodable by the CDBG wire format");
}

TEST(DashboardSessionBuilderTest, RejectsSelectedChannelsThatExceedCdbgFrameCapacity)
{
    auto document = test::valid_document();
    document.channels.clear();
    document.cards.clear();
    for (std::uint32_t index = 0; index < 57; ++index)
    {
        DashboardChannel channel = coolant_channel();
        channel.id = "channel-" + std::to_string(index);
        channel.address += index;
        document.channels.push_back(std::move(channel));
        document.cards.push_back(
            numeric_card("card-" + std::to_string(index), document.channels.back().id, "celsius", index));
    }

    expect_invalid(document, "cards: logging channels do not fit the selected protocol");
}
} // namespace
} // namespace fastecu::dashboard
