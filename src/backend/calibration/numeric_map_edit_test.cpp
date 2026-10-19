#include "src/backend/calibration/numeric_map_edit.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <array>
#include <limits>
#include <vector>

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

MapElementSpec SpecFor(StorageType storage)
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
    const auto positive = CalculateAssignment(rom, SpecFor(StorageType::kInt16), 1, cells, kOneCell, "10.5");
    ASSERT_THAT(positive, IsOk());
    EXPECT_THAT(positive->writes, ElementsAre(Field(&CellWrite::bytes, ElementsAre(0, 11))));
    const auto negative = CalculateAssignment(rom, SpecFor(StorageType::kInt16), 1, cells, kOneCell, "-10.5");
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
    const auto spec = SpecFor(example.storage);
    const bytes::Bytes rom(example.minimum_bytes.size(), 1);
    const std::array<NumericCell, 1> cells{0.0};
    const auto minimum = CalculateAssignment(rom, spec, 1, cells, kOneCell, example.minimum);
    ASSERT_THAT(minimum, IsOk());
    EXPECT_THAT(minimum->writes, ElementsAre(Field(&CellWrite::bytes, example.minimum_bytes)));
    const auto maximum = CalculateAssignment(rom, spec, 1, cells, kOneCell, example.maximum);
    ASSERT_THAT(maximum, IsOk());
    EXPECT_THAT(maximum->writes, ElementsAre(Field(&CellWrite::bytes, example.maximum_bytes)));
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, example.below), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, example.above), IsErr(ErrorKind::kInvalidConfig));
}

INSTANTIATE_TEST_SUITE_P(
    AllWidths, NumericStorageLimits,
    ::testing::Values(
        StorageLimitCase{StorageType::kUint8, "0", "255", "-1", "256", {0}, {0xff}},
        StorageLimitCase{StorageType::kInt8, "-128", "127", "-129", "128", {0x80}, {0x7f}},
        StorageLimitCase{StorageType::kUint16, "0", "65535", "-1", "65536", {0, 0}, {0xff, 0xff}},
        StorageLimitCase{StorageType::kInt16, "-32768", "32767", "-32769", "32768", {0x80, 0}, {0x7f, 0xff}},
        StorageLimitCase{StorageType::kUint24, "0", "16777215", "-1", "16777216", {0, 0, 0}, {0xff, 0xff, 0xff}},
        StorageLimitCase{
            StorageType::kInt24, "-8388608", "8388607", "-8388609", "8388608", {0x80, 0, 0}, {0x7f, 0xff, 0xff}},
        StorageLimitCase{
            StorageType::kUint32, "0", "4294967295", "-1", "4294967296", {0, 0, 0, 0}, {0xff, 0xff, 0xff, 0xff}},
        StorageLimitCase{StorageType::kInt32,
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
    const auto spec = SpecFor(StorageType::kFloat);
    const auto patch = CalculateAssignment(rom, spec, 1, cells, kOneCell, "1.5");
    ASSERT_THAT(patch, IsOk());
    EXPECT_THAT(patch->writes, ElementsAre(Field(&CellWrite::bytes, ElementsAre(0x3f, 0xc0, 0, 0))));
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "1e100"), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "1/0"), IsErr(ErrorKind::kInvalidConfig));
}

TEST(NumericMapEdit, ClampsDefinitionBoundsBeforeEncoding)
{
    auto spec = SpecFor(StorageType::kUint8);
    spec.min_value = "0";
    spec.max_value = "100";
    const bytes::Bytes rom{0};
    const std::array<NumericCell, 1> cells{0.0};
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "120"),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(100))))));
    spec.min_value = "broken";
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "1"), IsErr(ErrorKind::kInvalidConfig));
    spec.min_value = "101";
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "1"), IsErr(ErrorKind::kInvalidConfig));
}

TEST(NumericMapEdit, AssignsInvalidCellWithoutReadingOldValue)
{
    auto spec = SpecFor(StorageType::kUint8);
    spec.from_byte = "1/0";
    const bytes::Bytes rom{0};
    const std::array<NumericCell, 1> cells{Fail(ErrorKind::kInvalidConfig, "invalid old value")};
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "20"),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(20))))));
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "x+20"), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(CalculatePaste(rom, spec, 1, 1, kOneCell, std::vector<std::vector<double>>{{21}}),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(21))))));
}

TEST(NumericMapEdit, RejectsEntireIncrementWithInvalidRequiredCell)
{
    auto spec = SpecFor(StorageType::kUint8);
    spec.fine_increment = 1;
    const bytes::Bytes rom{1, 2, 3};
    const std::array<NumericCell, 3> cells{1.0, Fail(ErrorKind::kInvalidConfig, "invalid"), 3.0};
    EXPECT_THAT(CalculateIncrement(rom, spec, 3, cells, kThreeCells, IncrementStep::kFineUp),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(rom, ElementsAre(1, 2, 3));
}

TEST(NumericMapEdit, AppliesSubResolutionIncrementOnce)
{
    auto spec = SpecFor(StorageType::kUint8);
    spec.to_byte = "x*10";
    spec.from_byte = "x/10";
    spec.fine_increment = 0.01;
    const bytes::Bytes rom{100};
    const std::array<NumericCell, 1> cells{10.0};
    EXPECT_THAT(CalculateIncrement(rom, spec, 1, cells, kOneCell, IncrementStep::kFineUp),
                IsOkAnd(WritesAre(::testing::IsEmpty())));
}

TEST(NumericMapEdit, ReportsDefinitionLimitWithoutClaimingStorageResolution)
{
    auto spec = SpecFor(StorageType::kUint8);
    spec.max_value = "100";
    spec.fine_increment = 1;
    const bytes::Bytes rom{100};
    const std::array<NumericCell, 1> cells{100.0};
    const auto result = CalculateIncrement(rom, spec, 1, cells, kOneCell, IncrementStep::kFineUp);
    ASSERT_THAT(result, IsOk());
    EXPECT_THAT(result->writes, ::testing::IsEmpty());
    EXPECT_EQ(result->no_change, NoChangeReason::kDefinitionLimit);
}

TEST(NumericMapEdit, ReportsSubResolutionNoChange)
{
    auto spec = SpecFor(StorageType::kUint8);
    spec.to_byte = "x*10";
    spec.fine_increment = 0.01;
    const bytes::Bytes rom{100};
    const std::array<NumericCell, 1> cells{10.0};
    const auto result = CalculateIncrement(rom, spec, 1, cells, kOneCell, IncrementStep::kFineUp);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->no_change, NoChangeReason::kBelowStorageResolution);
}

TEST(NumericMapEdit, UsesFullDoubleValueDespiteDisplayRounding)
{
    auto spec = SpecFor(StorageType::kUint16);
    spec.to_byte = "x*10000";
    const bytes::Bytes rom{0, 1};
    const std::array<NumericCell, 1> cells{0.0001};
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "x+0.01"),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(0, 101))))));
}

TEST(NumericMapEdit, InterpolatesAcrossInvalidInterior)
{
    const auto spec = SpecFor(StorageType::kUint8);
    const bytes::Bytes rom{1, 0, 5};
    std::array<NumericCell, 3> cells{1.0, Fail(ErrorKind::kInvalidConfig, "invalid"), 5.0};
    EXPECT_THAT(CalculateInterpolation(rom, spec, 3, cells, kThreeCells, InterpolationMode::kHorizontal),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(3))))));
    cells[0] = Fail(ErrorKind::kInvalidConfig, "invalid endpoint");
    EXPECT_THAT(CalculateInterpolation(rom, spec, 3, cells, kThreeCells, InterpolationMode::kHorizontal),
                IsErr(ErrorKind::kInvalidConfig));
}

TEST(NumericMapEdit, RejectsInvalidSelectionAndEncodingMetadata)
{
    auto spec = SpecFor(StorageType::kUint8);
    const bytes::Bytes rom{0};
    const std::array<NumericCell, 1> cells{0.0};
    const SelectionRange negative{.first_row = -1, .last_row = 0};
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, negative, "1"), IsErr(ErrorKind::kInvalidConfig));
    const SelectionRange past_end{.first_col = 1, .last_col = 1};
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, past_end, "1"), IsErr(ErrorKind::kInvalidConfig));
    spec.to_byte = "1/0";
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "1"), IsErr(ErrorKind::kInvalidConfig));
}

TEST(NumericMapEdit, RejectsNonFinitePastedValueBeforeReturningAnyWrites)
{
    const auto spec = SpecFor(StorageType::kUint8);
    const bytes::Bytes rom{0, 0};
    const std::vector<std::vector<double>> pasted{{1, std::numeric_limits<double>::infinity()}};
    EXPECT_THAT(CalculatePaste(rom, spec, 2, 1, kOneCell, pasted), IsErr(ErrorKind::kInvalidConfig));
}

TEST(NumericMapEdit, InterpolatesBidirectionallyFromFourValidCorners)
{
    const auto spec = SpecFor(StorageType::kUint8);
    const bytes::Bytes rom{0, 0, 10, 0, 0, 0, 20, 0, 30};
    const auto invalid = Fail(ErrorKind::kInvalidConfig, "invalid interior");
    std::array<NumericCell, 9> cells{0.0, invalid, 10.0, invalid, invalid, invalid, 20.0, invalid, 30.0};
    const SelectionRange range{.first_row = 0, .first_col = 0, .last_row = 2, .last_col = 2};
    const auto result = CalculateInterpolation(rom, spec, 3, cells, range, InterpolationMode::kBidirectional);
    ASSERT_THAT(result, IsOk());
    EXPECT_THAT(result->writes, ElementsAre(CellWrite{1, 1, {5}}, CellWrite{3, 3, {10}}, CellWrite{4, 4, {15}},
                                            CellWrite{5, 5, {20}}, CellWrite{7, 7, {25}}));
    cells[8] = invalid;
    EXPECT_THAT(CalculateInterpolation(rom, spec, 3, cells, range, InterpolationMode::kBidirectional),
                IsErr(ErrorKind::kInvalidConfig));
}

TEST(NumericMapEdit, InterpolatesVerticalRunWithoutReadingInvalidInterior)
{
    const auto spec = SpecFor(StorageType::kUint8);
    const bytes::Bytes rom{0, 0, 4};
    const std::array<NumericCell, 3> cells{0.0, Fail(ErrorKind::kInvalidConfig, "invalid interior"), 4.0};
    const SelectionRange range{.first_row = 0, .first_col = 0, .last_row = 2, .last_col = 0};
    EXPECT_THAT(CalculateInterpolation(rom, spec, 1, cells, range, InterpolationMode::kVertical),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{1, 1, {2}}))));
}

TEST(NumericMapEdit, PreservesRaggedPasteAndEdgeClipping)
{
    const auto spec = SpecFor(StorageType::kUint8);
    const bytes::Bytes rom{0, 0, 0, 0};
    const std::vector<std::vector<double>> values{{1, 2, 3}, {4}, {5, 6}};
    EXPECT_THAT(CalculatePaste(rom, spec, 2, 2, kOneCell, values),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, {1}}, CellWrite{1, 1, {2}}, CellWrite{2, 2, {4}}))));
}

TEST(NumericMapEdit, AllowsRepresentableSignedIncrementAcrossZero)
{
    auto spec = SpecFor(StorageType::kInt8);
    spec.fine_increment = 2;
    const bytes::Bytes rom{0xff};
    const std::array<NumericCell, 1> cells{-1.0};
    EXPECT_THAT(CalculateIncrement(rom, spec, 1, cells, kOneCell, IncrementStep::kFineUp),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, {1}}))));
}

TEST(NumericMapEdit, InterpolatesSelectionWiderThanLegacyFixedArray)
{
    const auto spec = SpecFor(StorageType::kUint8);
    bytes::Bytes rom(190, 0);
    rom.back() = 189;
    std::vector<NumericCell> cells(190, Fail(ErrorKind::kInvalidConfig, "invalid interior"));
    cells.front() = 0.0;
    cells.back() = 189.0;
    const SelectionRange range{.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 189};
    const auto result = CalculateInterpolation(rom, spec, 190, cells, range, InterpolationMode::kHorizontal);
    ASSERT_THAT(result, IsOk());
    ASSERT_THAT(result->writes, ::testing::SizeIs(188));
    EXPECT_EQ(result->writes.front().index, 1U);
    EXPECT_THAT(result->writes.front().bytes, ElementsAre(1));
    EXPECT_EQ(result->writes.back().index, 188U);
    EXPECT_THAT(result->writes.back().bytes, ElementsAre(188));
}
TEST(NumericMapEdit, AcceptsEqualDefinitionLimits)
{
    auto spec = SpecFor(StorageType::kUint8);
    spec.min_value = "5";
    spec.max_value = "5";
    const bytes::Bytes rom{0};
    const std::array<NumericCell, 1> cells{0.0};
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "9"),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, {5}}))));
}

TEST(NumericMapEdit, AcceptsLeadingNineInAssignment)
{
    const bytes::Bytes rom{0};
    const std::array<NumericCell, 1> cells{0.0};
    EXPECT_THAT(CalculateAssignment(rom, SpecFor(StorageType::kUint8), 1, cells, kOneCell, "9"),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, {9}}))));
}

TEST(NumericMapEdit, AcceptsLargestFiniteFloat)
{
    const bytes::Bytes rom(4, 0);
    const std::vector<std::vector<double>> pasted{{static_cast<double>(std::numeric_limits<float>::max())}};
    EXPECT_THAT(CalculatePaste(rom, SpecFor(StorageType::kFloat), 1, 1, kOneCell, pasted),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, {0x7f, 0x7f, 0xff, 0xff}}))));
}

TEST(NumericMapEdit, AcceptsRunWithExactlyMaxUint32Cells)
{
    const bytes::Bytes rom{0};
    const std::vector<std::vector<double>> pasted{{1}};
    EXPECT_THAT(CalculatePaste(rom, SpecFor(StorageType::kUint8), 65537, 65535, kOneCell, pasted),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, {1}}))));
}

TEST(NumericMapEdit, RejectsSelectionRowsPastRun)
{
    const auto spec = SpecFor(StorageType::kUint8);
    const bytes::Bytes rom{0, 0};
    const std::vector<std::vector<double>> pasted{{1}};
    const SelectionRange one_past_single_column{.first_row = 1, .first_col = 0, .last_row = 1, .last_col = 0};
    EXPECT_THAT(CalculatePaste(rom, spec, 1, 1, one_past_single_column, pasted), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(CalculatePaste(rom, spec, 2, 1, one_past_single_column, pasted), IsErr(ErrorKind::kInvalidConfig));
}

TEST(NumericMapEdit, ClipsPasteAtRunEdgesFromOffsetSelection)
{
    const auto spec = SpecFor(StorageType::kUint8);
    const bytes::Bytes rows_rom{0, 0, 0};
    const std::vector<std::vector<double>> rows{{1}, {2}, {3}};
    const SelectionRange second_row{.first_row = 1, .first_col = 0, .last_row = 1, .last_col = 0};
    EXPECT_THAT(CalculatePaste(rows_rom, spec, 1, 3, second_row, rows),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{1, 1, {1}}, CellWrite{2, 2, {2}}))));
    const bytes::Bytes columns_rom{0, 0, 0};
    const std::vector<std::vector<double>> columns{{1, 2, 3}};
    const SelectionRange second_column{.first_row = 0, .first_col = 1, .last_row = 0, .last_col = 1};
    EXPECT_THAT(CalculatePaste(columns_rom, spec, 3, 1, second_column, columns),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{1, 1, {1}}, CellWrite{2, 2, {2}}))));
}

// 4x4 run whose cells outside the selected block are valid decoys, so reading
// a wrong cell changes the result instead of only failing.
class OffsetInterpolation : public ::testing::Test
{
  protected:
    static constexpr std::uint32_t kWidth = 4;
    static constexpr SelectionRange kRange{.first_row = 1, .first_col = 1, .last_row = 3, .last_col = 3};

    OffsetInterpolation() : spec_(SpecFor(StorageType::kUint8)), rom_(16, 99), cells_(16, 99.0)
    {
        for (std::size_t row = 1; row <= 3; ++row)
        {
            for (std::size_t col = 1; col <= 3; ++col)
            {
                Set(row, col, 0, Fail(ErrorKind::kInvalidConfig, "invalid interior"));
            }
        }
    }

    void Set(std::size_t row, std::size_t col, std::uint8_t byte, NumericCell cell)
    {
        rom_[row * kWidth + col] = byte;
        cells_[row * kWidth + col] = std::move(cell);
    }

    void SetValid(std::size_t row, std::size_t col, std::uint8_t value)
    {
        Set(row, col, value, static_cast<double>(value));
    }

    MapElementSpec spec_;
    bytes::Bytes rom_;
    std::vector<NumericCell> cells_;
};

TEST_F(OffsetInterpolation, HorizontalUsesSelectionOffsets)
{
    for (std::uint8_t row = 1; row <= 3; ++row)
    {
        SetValid(row, 1, 10 * row);
        SetValid(row, 3, 10 * row + 20);
    }
    EXPECT_THAT(
        CalculateInterpolation(rom_, spec_, kWidth, cells_, kRange, InterpolationMode::kHorizontal),
        IsOkAnd(WritesAre(ElementsAre(CellWrite{6, 6, {20}}, CellWrite{10, 10, {30}}, CellWrite{14, 14, {40}}))));
}

TEST_F(OffsetInterpolation, VerticalUsesSelectionOffsets)
{
    for (std::uint8_t col = 1; col <= 3; ++col)
    {
        SetValid(1, col, 10 * col);
        SetValid(3, col, 10 * col + 20);
    }
    EXPECT_THAT(
        CalculateInterpolation(rom_, spec_, kWidth, cells_, kRange, InterpolationMode::kVertical),
        IsOkAnd(WritesAre(ElementsAre(CellWrite{9, 9, {20}}, CellWrite{10, 10, {30}}, CellWrite{11, 11, {40}}))));
}

TEST_F(OffsetInterpolation, BidirectionalUsesSelectionOffsets)
{
    SetValid(1, 1, 0);
    SetValid(1, 3, 20);
    SetValid(3, 1, 40);
    SetValid(3, 3, 60);
    EXPECT_THAT(CalculateInterpolation(rom_, spec_, kWidth, cells_, kRange, InterpolationMode::kBidirectional),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{6, 6, {10}}, CellWrite{9, 9, {20}}, CellWrite{10, 10, {30}},
                                              CellWrite{11, 11, {40}}, CellWrite{14, 14, {50}}))));
}
} // namespace
} // namespace fastecu::calibration
