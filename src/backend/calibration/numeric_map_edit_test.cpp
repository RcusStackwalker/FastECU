#include "src/backend/calibration/numeric_map_edit.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <array>
#include <limits>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

namespace fastecu::calibration
{
namespace
{
using definition::StorageType;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::ElementsAre;
using ::testing::Field;

MATCHER_P(WritesAre, matcher, "")
{
    return ::testing::ExplainMatchResult(matcher, arg.writes, result_listener);
}

MapElementSpec spec_for(StorageType storage)
{
    MapElementSpec spec;
    spec.storage_type = storage;
    spec.endian = "big";
    return spec;
}

constexpr SelectionRange kOneCell{.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0};
constexpr SelectionRange kThreeCells{.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 2};

TEST(NumericMapEdit, RoundsHalfAwayFromZero)
{
    const bytes::Bytes rom{0, 0};
    const std::array<NumericCell, 1> cells{0.0};
    const auto positive = calculate_assignment(rom, spec_for(StorageType::Int16), 1, cells, kOneCell, "10.5");
    ASSERT_THAT(positive, IsOk());
    EXPECT_THAT(positive->writes, ElementsAre(Field(&CellWrite::bytes, ElementsAre(0, 11))));
    const auto negative = calculate_assignment(rom, spec_for(StorageType::Int16), 1, cells, kOneCell, "-10.5");
    ASSERT_THAT(negative, IsOk());
    EXPECT_THAT(negative->writes, ElementsAre(Field(&CellWrite::bytes, ElementsAre(0xff, 0xf5))));
}

struct StorageLimitCase
{
    StorageType storage;
    std::string_view minimum;
    std::string_view maximum;
    std::string_view below;
    std::string_view above;
    bytes::Bytes minimum_bytes;
    bytes::Bytes maximum_bytes;
};

class NumericStorageLimits : public ::testing::TestWithParam<StorageLimitCase>
{
};

TEST_P(NumericStorageLimits, ChecksExactLimitsBeforeNarrowing)
{
    const auto& example = GetParam();
    const auto spec = spec_for(example.storage);
    const bytes::Bytes rom(example.minimum_bytes.size(), 1);
    const std::array<NumericCell, 1> cells{0.0};
    const auto minimum = calculate_assignment(rom, spec, 1, cells, kOneCell, example.minimum);
    ASSERT_THAT(minimum, IsOk());
    EXPECT_THAT(minimum->writes, ElementsAre(Field(&CellWrite::bytes, example.minimum_bytes)));
    const auto maximum = calculate_assignment(rom, spec, 1, cells, kOneCell, example.maximum);
    ASSERT_THAT(maximum, IsOk());
    EXPECT_THAT(maximum->writes, ElementsAre(Field(&CellWrite::bytes, example.maximum_bytes)));
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, example.below), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, example.above), IsErr(ErrorKind::InvalidConfig));
}

INSTANTIATE_TEST_SUITE_P(
    AllWidths, NumericStorageLimits,
    ::testing::Values(
        StorageLimitCase{StorageType::Uint8, "0", "255", "-1", "256", {0}, {0xff}},
        StorageLimitCase{StorageType::Int8, "-128", "127", "-129", "128", {0x80}, {0x7f}},
        StorageLimitCase{StorageType::Uint16, "0", "65535", "-1", "65536", {0, 0}, {0xff, 0xff}},
        StorageLimitCase{StorageType::Int16, "-32768", "32767", "-32769", "32768", {0x80, 0}, {0x7f, 0xff}},
        StorageLimitCase{StorageType::Uint24, "0", "16777215", "-1", "16777216", {0, 0, 0}, {0xff, 0xff, 0xff}},
        StorageLimitCase{
            StorageType::Int24, "-8388608", "8388607", "-8388609", "8388608", {0x80, 0, 0}, {0x7f, 0xff, 0xff}},
        StorageLimitCase{
            StorageType::Uint32, "0", "4294967295", "-1", "4294967296", {0, 0, 0, 0}, {0xff, 0xff, 0xff, 0xff}},
        StorageLimitCase{StorageType::Int32,
                         "-2147483648",
                         "2147483647",
                         "-2147483649",
                         "2147483648",
                         {0x80, 0, 0, 0},
                         {0x7f, 0xff, 0xff, 0xff}}));

TEST(NumericMapEdit, EncodesFloatRepresentationAndRejectsOverflow)
{
    const bytes::Bytes rom(4, 0);
    const std::array<NumericCell, 1> cells{0.0};
    const auto spec = spec_for(StorageType::Float);
    const auto patch = calculate_assignment(rom, spec, 1, cells, kOneCell, "1.5");
    ASSERT_THAT(patch, IsOk());
    EXPECT_THAT(patch->writes, ElementsAre(Field(&CellWrite::bytes, ElementsAre(0x3f, 0xc0, 0, 0))));
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, "1e100"), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, "1/0"), IsErr(ErrorKind::InvalidConfig));
}

TEST(NumericMapEdit, ClampsDefinitionBoundsBeforeEncoding)
{
    auto spec = spec_for(StorageType::Uint8);
    spec.min_value = "0";
    spec.max_value = "100";
    const bytes::Bytes rom{0};
    const std::array<NumericCell, 1> cells{0.0};
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, "120"),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(100))))));
    spec.min_value = "broken";
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, "1"), IsErr(ErrorKind::InvalidConfig));
    spec.min_value = "101";
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, "1"), IsErr(ErrorKind::InvalidConfig));
}

TEST(NumericMapEdit, AssignsInvalidCellWithoutReadingOldValue)
{
    auto spec = spec_for(StorageType::Uint8);
    spec.from_byte = "1/0";
    const bytes::Bytes rom{0};
    const std::array<NumericCell, 1> cells{fail(ErrorKind::InvalidConfig, "invalid old value")};
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, "20"),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(20))))));
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, "x+20"), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(calculate_paste(rom, spec, 1, 1, kOneCell, std::vector<std::vector<double>>{{21}}),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(21))))));
}

TEST(NumericMapEdit, RejectsEntireIncrementWithInvalidRequiredCell)
{
    auto spec = spec_for(StorageType::Uint8);
    spec.fine_increment = 1;
    const bytes::Bytes rom{1, 2, 3};
    const std::array<NumericCell, 3> cells{1.0, fail(ErrorKind::InvalidConfig, "invalid"), 3.0};
    EXPECT_THAT(calculate_increment(rom, spec, 3, cells, kThreeCells, IncrementStep::FineUp),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(rom, ElementsAre(1, 2, 3));
}

TEST(NumericMapEdit, AppliesSubResolutionIncrementOnce)
{
    auto spec = spec_for(StorageType::Uint8);
    spec.to_byte = "x*10";
    spec.from_byte = "x/10";
    spec.fine_increment = 0.01;
    const bytes::Bytes rom{100};
    const std::array<NumericCell, 1> cells{10.0};
    EXPECT_THAT(calculate_increment(rom, spec, 1, cells, kOneCell, IncrementStep::FineUp),
                IsOkAnd(WritesAre(::testing::IsEmpty())));
}

TEST(NumericMapEdit, ReportsDefinitionLimitWithoutClaimingStorageResolution)
{
    auto spec = spec_for(StorageType::Uint8);
    spec.max_value = "100";
    spec.fine_increment = 1;
    const bytes::Bytes rom{100};
    const std::array<NumericCell, 1> cells{100.0};
    const auto result = calculate_increment(rom, spec, 1, cells, kOneCell, IncrementStep::FineUp);
    ASSERT_THAT(result, IsOk());
    EXPECT_THAT(result->writes, ::testing::IsEmpty());
    EXPECT_EQ(result->no_change, NoChangeReason::DefinitionLimit);
}

TEST(NumericMapEdit, ReportsSubResolutionNoChange)
{
    auto spec = spec_for(StorageType::Uint8);
    spec.to_byte = "x*10";
    spec.fine_increment = 0.01;
    const bytes::Bytes rom{100};
    const std::array<NumericCell, 1> cells{10.0};
    const auto result = calculate_increment(rom, spec, 1, cells, kOneCell, IncrementStep::FineUp);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->no_change, NoChangeReason::BelowStorageResolution);
}

TEST(NumericMapEdit, UsesFullDoubleValueDespiteDisplayRounding)
{
    auto spec = spec_for(StorageType::Uint16);
    spec.to_byte = "x*10000";
    const bytes::Bytes rom{0, 1};
    const std::array<NumericCell, 1> cells{0.0001};
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, "x+0.01"),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(0, 101))))));
}

TEST(NumericMapEdit, InterpolatesAcrossInvalidInterior)
{
    const auto spec = spec_for(StorageType::Uint8);
    const bytes::Bytes rom{1, 0, 5};
    std::array<NumericCell, 3> cells{1.0, fail(ErrorKind::InvalidConfig, "invalid"), 5.0};
    EXPECT_THAT(calculate_interpolation(rom, spec, 3, cells, kThreeCells, InterpolationMode::Horizontal),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(3))))));
    cells[0] = fail(ErrorKind::InvalidConfig, "invalid endpoint");
    EXPECT_THAT(calculate_interpolation(rom, spec, 3, cells, kThreeCells, InterpolationMode::Horizontal),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(NumericMapEdit, RejectsInvalidSelectionAndEncodingMetadata)
{
    auto spec = spec_for(StorageType::Uint8);
    const bytes::Bytes rom{0};
    const std::array<NumericCell, 1> cells{0.0};
    const SelectionRange negative{.first_row = -1, .last_row = 0};
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, negative, "1"), IsErr(ErrorKind::InvalidConfig));
    const SelectionRange past_end{.first_col = 1, .last_col = 1};
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, past_end, "1"), IsErr(ErrorKind::InvalidConfig));
    spec.to_byte = "1/0";
    EXPECT_THAT(calculate_assignment(rom, spec, 1, cells, kOneCell, "1"), IsErr(ErrorKind::InvalidConfig));
}

TEST(NumericMapEdit, RejectsNonFinitePastedValueBeforeReturningAnyWrites)
{
    const auto spec = spec_for(StorageType::Uint8);
    const bytes::Bytes rom{0, 0};
    const std::vector<std::vector<double>> pasted{{1, std::numeric_limits<double>::infinity()}};
    EXPECT_THAT(calculate_paste(rom, spec, 2, 1, kOneCell, pasted), IsErr(ErrorKind::InvalidConfig));
}

TEST(NumericMapEdit, InterpolatesBidirectionallyFromFourValidCorners)
{
    const auto spec = spec_for(StorageType::Uint8);
    const bytes::Bytes rom{0, 0, 10, 0, 0, 0, 20, 0, 30};
    const auto invalid = fail(ErrorKind::InvalidConfig, "invalid interior");
    std::array<NumericCell, 9> cells{0.0, invalid, 10.0, invalid, invalid, invalid, 20.0, invalid, 30.0};
    const SelectionRange range{.first_row = 0, .first_col = 0, .last_row = 2, .last_col = 2};
    const auto result = calculate_interpolation(rom, spec, 3, cells, range, InterpolationMode::Bidirectional);
    ASSERT_THAT(result, IsOk());
    EXPECT_THAT(result->writes, ElementsAre(CellWrite{1, 1, {5}}, CellWrite{3, 3, {10}}, CellWrite{4, 4, {15}},
                                            CellWrite{5, 5, {20}}, CellWrite{7, 7, {25}}));
    cells[8] = invalid;
    EXPECT_THAT(calculate_interpolation(rom, spec, 3, cells, range, InterpolationMode::Bidirectional),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(NumericMapEdit, InterpolatesVerticalRunWithoutReadingInvalidInterior)
{
    const auto spec = spec_for(StorageType::Uint8);
    const bytes::Bytes rom{0, 0, 4};
    const std::array<NumericCell, 3> cells{0.0, fail(ErrorKind::InvalidConfig, "invalid interior"), 4.0};
    const SelectionRange range{.first_row = 0, .first_col = 0, .last_row = 2, .last_col = 0};
    EXPECT_THAT(calculate_interpolation(rom, spec, 1, cells, range, InterpolationMode::Vertical),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{1, 1, {2}}))));
}

TEST(NumericMapEdit, PreservesRaggedPasteAndEdgeClipping)
{
    const auto spec = spec_for(StorageType::Uint8);
    const bytes::Bytes rom{0, 0, 0, 0};
    const std::vector<std::vector<double>> values{{1, 2, 3}, {4}, {5, 6}};
    EXPECT_THAT(calculate_paste(rom, spec, 2, 2, kOneCell, values),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, {1}}, CellWrite{1, 1, {2}}, CellWrite{2, 2, {4}}))));
}

TEST(NumericMapEdit, AllowsRepresentableSignedIncrementAcrossZero)
{
    auto spec = spec_for(StorageType::Int8);
    spec.fine_increment = 2;
    const bytes::Bytes rom{0xff};
    const std::array<NumericCell, 1> cells{-1.0};
    EXPECT_THAT(calculate_increment(rom, spec, 1, cells, kOneCell, IncrementStep::FineUp),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, {1}}))));
}

TEST(NumericMapEdit, InterpolatesSelectionWiderThanLegacyFixedArray)
{
    const auto spec = spec_for(StorageType::Uint8);
    bytes::Bytes rom(190, 0);
    rom.back() = 189;
    std::vector<NumericCell> cells(190, fail(ErrorKind::InvalidConfig, "invalid interior"));
    cells.front() = 0.0;
    cells.back() = 189.0;
    const SelectionRange range{.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 189};
    const auto result = calculate_interpolation(rom, spec, 190, cells, range, InterpolationMode::Horizontal);
    ASSERT_THAT(result, IsOk());
    ASSERT_THAT(result->writes, ::testing::SizeIs(188));
    EXPECT_EQ(result->writes.front().index, 1U);
    EXPECT_THAT(result->writes.front().bytes, ElementsAre(1));
    EXPECT_EQ(result->writes.back().index, 188U);
    EXPECT_THAT(result->writes.back().bytes, ElementsAre(188));
}
} // namespace
} // namespace fastecu::calibration
