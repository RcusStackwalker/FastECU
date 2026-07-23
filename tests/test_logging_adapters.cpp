#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"
#include "src/platform/desktop/common/logging/logging_value_adapter.h"

#include <functional>
#include <string>
#include <vector>

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

// Appends a row with an explicit, fully-formed "units" (conversion) field so
// malformed-conversion cases can hand in a deliberately broken string.
void append_value_with_units(FileActions::LogValuesStructure& values, const QString& id,
                             const QString& protocol, const QString& enabled,
                             const QString& units,
                             const QString& address = QStringLiteral("000010"),
                             const QString& length = QStringLiteral("1"))
{
    values.log_value_id.append(id);
    values.log_value_protocol.append(protocol);
    values.log_value_name.append(id);
    values.log_value_description.append(id);
    values.log_value_ecu_byte_index.append(QStringLiteral("0"));
    values.log_value_ecu_bit.append(QStringLiteral("0"));
    values.log_value_target.append(QStringLiteral("ECU"));
    values.log_value_address.append(address);
    values.log_value_units.append(units);
    values.log_value_length.append(length);
    values.log_value.append(QStringLiteral("unchanged- ") + id);
    values.log_value_enabled.append(enabled);
}

void append_value(FileActions::LogValuesStructure& values, const QString& id,
                  const QString& protocol, const QString& enabled,
                  const QString& format = QStringLiteral("0.00"))
{
    append_value_with_units(values, id, protocol, enabled,
                            QStringLiteral("conversion 0,rpm,x,") + format +
                                QStringLiteral(",0,100,1"));
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
    append_value(values, QStringLiteral("other-protocol"), QStringLiteral("OTHER"), QStringLiteral("1"));
    append_value(values, QStringLiteral("ssm-disabled"), QStringLiteral("CAR_SSM"), QStringLiteral("0"));
    append_value(values, QStringLiteral("ssm-enabled"), QStringLiteral("CAR_SSM"), QStringLiteral("1"));
    values.lower_panel_log_value_id = {
        QStringLiteral("other-protocol"),
        QStringLiteral("ssm-disabled"),
        QStringLiteral("ssm-enabled"),
    };
    const auto snapshot = desktop_logging::make_desktop_logging_snapshot(
        values, portable_logging::LoggingProtocolId::Ssm, QStringLiteral("CAR_SSM"), valid_policy());

    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->session.channels().size(), 2u);
    EXPECT_EQ(snapshot->session.channels().at(0).id, "ssm-disabled");
    EXPECT_EQ(snapshot->session.channels().at(1).id, "ssm-enabled");
    EXPECT_EQ(snapshot->response_offsets, (std::vector<std::size_t>{1, 2}));
    EXPECT_FALSE(snapshot->enabled_ids.contains("ssm-disabled"));
    EXPECT_TRUE(snapshot->enabled_ids.contains("ssm-enabled"));

    const portable_logging::LogSample disabled_sample{
        .channel_id = "ssm-disabled", .numeric_value = 1.0, .raw_value = "1", .unit = "rpm"};
    const portable_logging::LogSample enabled_sample{
        .channel_id = "ssm-enabled", .numeric_value = 2.0, .raw_value = "2", .unit = "rpm"};
    ASSERT_TRUE(desktop_logging::apply_log_sample(*snapshot, disabled_sample, values).has_value());
    ASSERT_TRUE(desktop_logging::apply_log_sample(*snapshot, enabled_sample, values).has_value());
    EXPECT_EQ(values.log_value.at(1), QStringLiteral("unchanged- ssm-disabled"));
    EXPECT_EQ(values.log_value.at(2), QStringLiteral("2.00"));
}

TEST(DesktopLoggingValueAdapterTest, DisabledSsmSampleRejectsMutatedSnapshotRow)
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

    values.log_value_id.replace(0, QStringLiteral("reordered-other-row"));
    const portable_logging::LogSample sample{
        .channel_id = "ssm-disabled", .numeric_value = 1.0, .raw_value = "1", .unit = "rpm"};
    const auto status = desktop_logging::apply_log_sample(*snapshot, sample, values);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_EQ(values.log_value.at(0), QStringLiteral("unchanged- ssm-disabled"));
    EXPECT_EQ(values.log_value.at(1), QStringLiteral("unchanged- ssm-enabled"));
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

TEST(DesktopLoggingSnapshotAdapterTest, AllowsSameOpaqueIdInDifferentProtocols)
{
    FileActions::LogValuesStructure values;
    append_value(values, QStringLiteral("rpm"), QStringLiteral("SSM"), QStringLiteral("1"));
    append_value(values, QStringLiteral("rpm"), QStringLiteral("CDBG"), QStringLiteral("1"));
    values.lower_panel_log_value_id = {QStringLiteral("rpm")};

    const auto snapshot = desktop_logging::make_desktop_logging_snapshot(
        values, portable_logging::LoggingProtocolId::Ssm, QStringLiteral("SSM"), valid_policy());

    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->session.channels().size(), 1u);
    EXPECT_EQ(snapshot->index_by_id.at("rpm"), 0);
}

TEST(DesktopLoggingSnapshotAdapterTest, RejectsDuplicateOpaqueIdWithinSelectedProtocol)
{
    FileActions::LogValuesStructure values;
    append_value(values, QStringLiteral("rpm"), QStringLiteral("SSM"), QStringLiteral("1"));
    append_value(values, QStringLiteral("rpm"), QStringLiteral("SSM"), QStringLiteral("1"));
    values.lower_panel_log_value_id = {QStringLiteral("rpm")};

    const auto snapshot = desktop_logging::make_desktop_logging_snapshot(
        values, portable_logging::LoggingProtocolId::Ssm, QStringLiteral("SSM"), valid_policy());

    ASSERT_FALSE(snapshot.has_value());
    EXPECT_EQ(snapshot.error().kind, fastecu::ErrorKind::InvalidConfig);
}

namespace
{

// Table-driven coverage of the legacy-input validation/error paths in
// make_desktop_logging_snapshot: each case builds a deliberately invalid
// FileActions::LogValuesStructure (or protocol/filter pairing) and asserts
// both the resulting ErrorKind and a substring of the human-readable detail,
// so the assertion is tied to the specific validation branch it targets.
struct SnapshotFailureCase
{
    const char *name;
    std::function<FileActions::LogValuesStructure()> build_values;
    portable_logging::LoggingProtocolId protocol;
    QString protocol_filter;
    fastecu::ErrorKind expected_kind;
    std::string expected_detail_substring;
};

std::vector<SnapshotFailureCase> snapshot_failure_cases()
{
    return {
        {
            "EmptySsmProtocolFilter",
            []
            {
                FileActions::LogValuesStructure values;
                append_value(values, QStringLiteral("rpm"), QStringLiteral("SSM"),
                             QStringLiteral("1"));
                values.lower_panel_log_value_id = {QStringLiteral("rpm")};
                return values;
            },
            portable_logging::LoggingProtocolId::Ssm,
            QString(),
            fastecu::ErrorKind::InvalidConfig,
            "protocol filter is empty",
        },
        {
            "UnknownProtocolId",
            []
            {
                FileActions::LogValuesStructure values;
                append_value(values, QStringLiteral("rpm"), QStringLiteral("SSM"),
                             QStringLiteral("1"));
                values.lower_panel_log_value_id = {QStringLiteral("rpm")};
                return values;
            },
            static_cast<portable_logging::LoggingProtocolId>(99),
            QStringLiteral("SSM"),
            fastecu::ErrorKind::InvalidConfig,
            "invalid logging protocol",
        },
        {
            "MalformedConversionTooFewFields",
            []
            {
                FileActions::LogValuesStructure values;
                append_value_with_units(values, QStringLiteral("rpm"), QStringLiteral("SSM"),
                                        QStringLiteral("1"),
                                        QStringLiteral("conversion 0,rpm,x"));
                values.lower_panel_log_value_id = {QStringLiteral("rpm")};
                return values;
            },
            portable_logging::LoggingProtocolId::Ssm,
            QStringLiteral("SSM"),
            fastecu::ErrorKind::InvalidConfig,
            "malformed logging conversion",
        },
        {
            "MalformedConversionEmptyByteExpression",
            []
            {
                FileActions::LogValuesStructure values;
                append_value_with_units(
                    values, QStringLiteral("rpm"), QStringLiteral("SSM"), QStringLiteral("1"),
                    QStringLiteral("conversion 0,rpm,,0.00,0,100,1"));
                values.lower_panel_log_value_id = {QStringLiteral("rpm")};
                return values;
            },
            portable_logging::LoggingProtocolId::Ssm,
            QStringLiteral("SSM"),
            fastecu::ErrorKind::InvalidConfig,
            "malformed logging conversion",
        },
        {
            "InvalidHexAddress",
            []
            {
                FileActions::LogValuesStructure values;
                append_value_with_units(values, QStringLiteral("rpm"), QStringLiteral("SSM"),
                                        QStringLiteral("1"),
                                        QStringLiteral("conversion 0,rpm,x,0.00,0,100,1"),
                                        QStringLiteral("zzzzzz"));
                values.lower_panel_log_value_id = {QStringLiteral("rpm")};
                return values;
            },
            portable_logging::LoggingProtocolId::Ssm,
            QStringLiteral("SSM"),
            fastecu::ErrorKind::InvalidConfig,
            "invalid logging address or length",
        },
        {
            "InvalidDecimalLength",
            []
            {
                FileActions::LogValuesStructure values;
                append_value_with_units(
                    values, QStringLiteral("rpm"), QStringLiteral("SSM"), QStringLiteral("1"),
                    QStringLiteral("conversion 0,rpm,x,0.00,0,100,1"),
                    QStringLiteral("000010"), QStringLiteral("not-a-number"));
                values.lower_panel_log_value_id = {QStringLiteral("rpm")};
                return values;
            },
            portable_logging::LoggingProtocolId::Ssm,
            QStringLiteral("SSM"),
            fastecu::ErrorKind::InvalidConfig,
            "invalid logging address or length",
        },
        {
            "PrecisionExceedsUint8Max",
            []
            {
                FileActions::LogValuesStructure values;
                const QString format = QStringLiteral("0.") + QString(300, QChar('0'));
                append_value(values, QStringLiteral("rpm"), QStringLiteral("SSM"),
                             QStringLiteral("1"), format);
                values.lower_panel_log_value_id = {QStringLiteral("rpm")};
                return values;
            },
            portable_logging::LoggingProtocolId::Ssm,
            QStringLiteral("SSM"),
            fastecu::ErrorKind::InvalidConfig,
            "precision is too large",
        },
        {
            "StructurallyInconsistentValueLists",
            []
            {
                FileActions::LogValuesStructure values;
                append_value(values, QStringLiteral("rpm"), QStringLiteral("SSM"),
                             QStringLiteral("1"));
                // Appends an id with no matching entry in the other parallel
                // lists, breaking the list-length invariant.
                values.log_value_id.append(QStringLiteral("stray"));
                values.lower_panel_log_value_id = {QStringLiteral("rpm")};
                return values;
            },
            portable_logging::LoggingProtocolId::Ssm,
            QStringLiteral("SSM"),
            fastecu::ErrorKind::InvalidConfig,
            "malformed legacy logging value lists",
        },
        {
            "ChannelValidationFailureFromInvalidByteExpression",
            []
            {
                FileActions::LogValuesStructure values;
                append_value_with_units(
                    values, QStringLiteral("rpm"), QStringLiteral("SSM"), QStringLiteral("1"),
                    QStringLiteral("conversion 0,rpm,not_an_expr,0.00,0,100,1"));
                values.lower_panel_log_value_id = {QStringLiteral("rpm")};
                return values;
            },
            portable_logging::LoggingProtocolId::Ssm,
            QStringLiteral("SSM"),
            fastecu::ErrorKind::InvalidConfig,
            "invalid logging channel",
        },
        {
            "SessionValidationFailureFromEmptyCdbgSelection",
            []
            {
                FileActions::LogValuesStructure values;
                append_value(values, QStringLiteral("ssm-only"), QStringLiteral("SSM"),
                             QStringLiteral("1"));
                values.lower_panel_log_value_id = {QStringLiteral("ssm-only")};
                return values;
            },
            portable_logging::LoggingProtocolId::Cdbg,
            QStringLiteral("ignored"),
            fastecu::ErrorKind::InvalidConfig,
            "no CDBG log parameters selected",
        },
        {
            "DuplicateLowerPanelId",
            []
            {
                FileActions::LogValuesStructure values;
                append_value(values, QStringLiteral("rpm"), QStringLiteral("SSM"),
                             QStringLiteral("1"));
                values.lower_panel_log_value_id = {QStringLiteral("rpm"), QStringLiteral("rpm")};
                return values;
            },
            portable_logging::LoggingProtocolId::Ssm,
            QStringLiteral("SSM"),
            fastecu::ErrorKind::InvalidConfig,
            "duplicate lower-panel logging value id",
        },
    };
}

void expect_snapshot_rejected(const SnapshotFailureCase& test_case)
{
    SCOPED_TRACE(test_case.name);
    const FileActions::LogValuesStructure values = test_case.build_values();
    const auto snapshot = desktop_logging::make_desktop_logging_snapshot(
        values, test_case.protocol, test_case.protocol_filter, valid_policy());

    ASSERT_FALSE(snapshot.has_value());
    EXPECT_EQ(snapshot.error().kind, test_case.expected_kind);
    EXPECT_NE(snapshot.error().detail.find(test_case.expected_detail_substring), std::string::npos)
        << "detail was: " << snapshot.error().detail;
}

} // namespace

TEST(DesktopLoggingSnapshotAdapterTest, RejectsInvalidLegacyInputs)
{
    for (const auto& test_case : snapshot_failure_cases())
    {
        ASSERT_NO_FATAL_FAILURE(expect_snapshot_rejected(test_case));
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
