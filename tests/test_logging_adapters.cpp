#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#include "src/platform/desktop/common/logging/logging_value_adapter.h"

#include <QString>
#include <gtest/gtest.h>

namespace desktop_logging = fastecu::desktop::logging;
namespace portable_logging = fastecu::logging;

namespace
{

portable_logging::LoggingPolicy valid_policy()
{
    return {
        .poll_timeout_ms = 100,
        .car_silence_miss_threshold = 3,
        .reconnect_attempt_threshold = 5,
        .reconnect_retry_period = 10,
    };
}

void append_value(FileActions::LogValuesStructure& values, const QString& id,
                  const QString& protocol, const QString& enabled,
                  const QString& format = QStringLiteral("0.00"))
{
    values.log_value_id.append(id);
    values.log_value_protocol.append(protocol);
    values.log_value_name.append(id);
    values.log_value_description.append(id);
    values.log_value_ecu_byte_index.append(QStringLiteral("0"));
    values.log_value_ecu_bit.append(QStringLiteral("0"));
    values.log_value_target.append(QStringLiteral("ECU"));
    values.log_value_address.append(QStringLiteral("000010"));
    values.log_value_units.append(
        QStringLiteral("conversion 0,rpm,x,") + format + QStringLiteral(",0,100,1"));
    values.log_value_length.append(QStringLiteral("1"));
    values.log_value.append(QStringLiteral("unchanged- ") + id);
    values.log_value_enabled.append(enabled);
}

FileActions::LogValuesStructure reordered_log_values()
{
    FileActions::LogValuesStructure values;
    append_value(values, QStringLiteral("coolant"), QStringLiteral("SSM"), QStringLiteral("1"));
    append_value(values, QStringLiteral("rpm"), QStringLiteral("SSM"), QStringLiteral("1"));
    values.lower_panel_log_value_id = {QStringLiteral("rpm"), QStringLiteral("coolant")};
    return values;
}

} // namespace

TEST(DesktopLoggingSnapshotAdapterTest, StableIdUpdatesOriginalRowAfterReorder)
{
    FileActions::LogValuesStructure values = reordered_log_values();
    auto snapshot = desktop_logging::make_desktop_logging_snapshot(
        values, portable_logging::LoggingProtocolId::Ssm, QStringLiteral("SSM"), valid_policy());

    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->index_by_id.at("rpm"), 1);
    const portable_logging::LogSample sample{
        .channel_id = "rpm",
        .numeric_value = 1234.5,
        .raw_value = "4938",
        .unit = "rpm",
    };

    ASSERT_TRUE(desktop_logging::apply_log_sample(*snapshot, sample, values).has_value());
    EXPECT_EQ(values.log_value.at(1), QStringLiteral("1234.50"));
    EXPECT_EQ(values.log_value.at(0), QStringLiteral("unchanged- coolant"));
}

TEST(DesktopLoggingValueAdapterTest, FormatsFixedDecimalEquivalently)
{
    EXPECT_EQ(desktop_logging::format_logging_value(2.0, 0), QStringLiteral("2"));
    EXPECT_EQ(desktop_logging::format_logging_value(-1.25, 2), QStringLiteral("-1.25"));
    EXPECT_EQ(desktop_logging::format_logging_value(1.2, 3), QStringLiteral("1.200"));
}

TEST(DesktopLoggingValueAdapterTest, MissingStableIdDoesNotUpdateAnotherRow)
{
    FileActions::LogValuesStructure values = reordered_log_values();
    auto snapshot = desktop_logging::make_desktop_logging_snapshot(
        values, portable_logging::LoggingProtocolId::Ssm, QStringLiteral("SSM"), valid_policy());
    ASSERT_TRUE(snapshot.has_value());

    const portable_logging::LogSample sample{
        .channel_id = "missing",
        .numeric_value = 1234.5,
        .raw_value = "4938",
        .unit = "rpm",
    };

    const auto status = desktop_logging::apply_log_sample(*snapshot, sample, values);
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_EQ(values.log_value.at(0), QStringLiteral("unchanged- coolant"));
    EXPECT_EQ(values.log_value.at(1), QStringLiteral("unchanged- rpm"));
}

TEST(DesktopLoggingSnapshotAdapterTest, PreservesLegacyProtocolSelectionRules)
{
    FileActions::LogValuesStructure values;
    append_value(values, QStringLiteral("ssm-disabled"), QStringLiteral("CAR_SSM"), QStringLiteral("0"));
    append_value(values, QStringLiteral("mut-disabled"), QStringLiteral("MUT_DMA"), QStringLiteral("0"));
    append_value(values, QStringLiteral("mut-enabled"), QStringLiteral("MUT_DMA"), QStringLiteral("1"));
    append_value(values, QStringLiteral("cdbg-disabled"), QStringLiteral("CDBG"), QStringLiteral("0"));
    values.lower_panel_log_value_id = {
        QStringLiteral("ssm-disabled"),
        QStringLiteral("mut-disabled"),
        QStringLiteral("mut-enabled"),
        QStringLiteral("cdbg-disabled"),
    };

    auto ssm = desktop_logging::make_desktop_logging_snapshot(
        values, portable_logging::LoggingProtocolId::Ssm, QStringLiteral("CAR_SSM"), valid_policy());
    auto mut = desktop_logging::make_desktop_logging_snapshot(
        values, portable_logging::LoggingProtocolId::MutDma, QStringLiteral("ignored"), valid_policy());
    auto cdbg = desktop_logging::make_desktop_logging_snapshot(
        values, portable_logging::LoggingProtocolId::Cdbg, QStringLiteral("ignored"), valid_policy());

    ASSERT_TRUE(ssm.has_value());
    ASSERT_TRUE(mut.has_value());
    ASSERT_TRUE(cdbg.has_value());
    EXPECT_EQ(ssm->session.channels().at(0).id, "ssm-disabled");
    ASSERT_EQ(mut->session.channels().size(), 1u);
    EXPECT_EQ(mut->session.channels().at(0).id, "mut-enabled");
    EXPECT_EQ(cdbg->session.channels().at(0).id, "cdbg-disabled");
}

TEST(DesktopLoggingSnapshotAdapterTest, SsmRetainsDisabledOffsetsButDoesNotUpdateDisabledRows)
{
    FileActions::LogValuesStructure values;
    append_value(values, QStringLiteral("ssm-disabled"), QStringLiteral("CAR_SSM"), QStringLiteral("0"));
    append_value(values, QStringLiteral("ssm-enabled"), QStringLiteral("CAR_SSM"), QStringLiteral("1"));
    values.lower_panel_log_value_id = {
        QStringLiteral("ssm-disabled"),
        QStringLiteral("ssm-enabled"),
    };
    const auto snapshot = desktop_logging::make_desktop_logging_snapshot(
        values, portable_logging::LoggingProtocolId::Ssm, QStringLiteral("CAR_SSM"), valid_policy());

    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->session.channels().size(), 2u);
    EXPECT_EQ(snapshot->session.channels().at(0).id, "ssm-disabled");
    EXPECT_EQ(snapshot->session.channels().at(1).id, "ssm-enabled");
    EXPECT_FALSE(snapshot->enabled_ids.contains("ssm-disabled"));
    EXPECT_TRUE(snapshot->enabled_ids.contains("ssm-enabled"));

    const portable_logging::LogSample disabled_sample{
        .channel_id = "ssm-disabled", .numeric_value = 1.0, .raw_value = "1", .unit = "rpm"};
    const portable_logging::LogSample enabled_sample{
        .channel_id = "ssm-enabled", .numeric_value = 2.0, .raw_value = "2", .unit = "rpm"};
    ASSERT_TRUE(desktop_logging::apply_log_sample(*snapshot, disabled_sample, values).has_value());
    ASSERT_TRUE(desktop_logging::apply_log_sample(*snapshot, enabled_sample, values).has_value());
    EXPECT_EQ(values.log_value.at(0), QStringLiteral("unchanged- ssm-disabled"));
    EXPECT_EQ(values.log_value.at(1), QStringLiteral("2.00"));
}

TEST(DesktopLoggingSnapshotAdapterTest, RejectsDuplicateStableIds)
{
    FileActions::LogValuesStructure values = reordered_log_values();
    append_value(values, QStringLiteral("rpm"), QStringLiteral("SSM"), QStringLiteral("1"));

    const auto snapshot = desktop_logging::make_desktop_logging_snapshot(
        values, portable_logging::LoggingProtocolId::Ssm, QStringLiteral("SSM"), valid_policy());

    ASSERT_FALSE(snapshot.has_value());
    EXPECT_EQ(snapshot.error().kind, fastecu::ErrorKind::InvalidConfig);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
