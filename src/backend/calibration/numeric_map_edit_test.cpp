#include "src/backend/calibration/numeric_map_edit.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

namespace fastecu::calibration
{
namespace
{
using definition::StorageType;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
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

// A ROM byte no expected encoding equals, so every encoding is a real write.
constexpr std::uint8_t kFiller = 0x55;

bytes::Bytes Filler(std::size_t size)
{
    return bytes::Bytes(size, kFiller);
}

constexpr SelectionRange kOneCell{.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0};
constexpr SelectionRange kThreeCells{.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 2};

// A nullopt expectation means the edit must be rejected.
using Encoding = std::optional<bytes::Bytes>;

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
                         {0x7f, 0xff, 0xff, 0xff}},
        // FLT_MAX is exact in a double; 3.4028235e38 is the next literal above it.
        StorageLimitCase{StorageType::kFloat,
                         "-3.4028234663852886e38",
                         "3.4028234663852886e38",
                         "-3.4028235e38",
                         "3.4028235e38",
                         {0xff, 0x7f, 0xff, 0xff},
                         {0x7f, 0x7f, 0xff, 0xff}}),
    (
        [](const auto& param_info)
        {
            static constexpr std::array kNames{"Uint8", "Int8",   "Uint16", "Int16", "Uint24",
                                               "Int24", "Uint32", "Int32",  "Float"};
            return kNames[param_info.index];
        }));

struct LiteralCase
{
    StorageType storage;
    std::string_view formula;
    Encoding encoded;
};

class AssignedLiterals : public ::testing::TestWithParam<LiteralCase>
{
};

TEST_P(AssignedLiterals, EncodesOrRejects)
{
    const auto& example = GetParam();
    const auto rom = Filler(4);
    const std::array<NumericCell, 1> cells{0.0};
    const auto result = CalculateAssignment(rom, SpecFor(example.storage), 1, cells, kOneCell, example.formula);
    if (!example.encoded.has_value())
    {
        EXPECT_THAT(result, IsErr(ErrorKind::kInvalidConfig));
        return;
    }
    EXPECT_THAT(result, IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, *example.encoded}))));
}

INSTANTIATE_TEST_SUITE_P(
    EveryLeadingDigit, AssignedLiterals,
    ::testing::Values(
        LiteralCase{StorageType::kUint8, "0", bytes::Bytes{0}}, LiteralCase{StorageType::kUint8, "1", bytes::Bytes{1}},
        LiteralCase{StorageType::kUint8, "2", bytes::Bytes{2}}, LiteralCase{StorageType::kUint8, "3", bytes::Bytes{3}},
        LiteralCase{StorageType::kUint8, "4", bytes::Bytes{4}}, LiteralCase{StorageType::kUint8, "5", bytes::Bytes{5}},
        LiteralCase{StorageType::kUint8, "6", bytes::Bytes{6}}, LiteralCase{StorageType::kUint8, "7", bytes::Bytes{7}},
        LiteralCase{StorageType::kUint8, "8", bytes::Bytes{8}}, LiteralCase{StorageType::kUint8, "9", bytes::Bytes{9}},
        LiteralCase{StorageType::kUint8, "95", bytes::Bytes{95}},
        LiteralCase{StorageType::kUint8, "9.5", bytes::Bytes{10}}),
    (
        [](const auto& param_info)
        {
            static constexpr std::array kNames{"Zero", "One",   "Two",   "Three", "Four",       "Five",
                                               "Six",  "Seven", "Eight", "Nine",  "NinetyFive", "FractionalRoundUp"};
            return kNames[param_info.index];
        }));

INSTANTIATE_TEST_SUITE_P(RoundsHalfAwayFromZero, AssignedLiterals,
                         ::testing::Values(LiteralCase{StorageType::kInt16, "0.5", bytes::Bytes{0, 1}},
                                           LiteralCase{StorageType::kInt16, "-0.5", bytes::Bytes{0xff, 0xff}},
                                           LiteralCase{StorageType::kInt16, "2.5", bytes::Bytes{0, 3}},
                                           LiteralCase{StorageType::kInt16, "-2.5", bytes::Bytes{0xff, 0xfd}},
                                           LiteralCase{StorageType::kInt16, "10.5", bytes::Bytes{0, 11}},
                                           LiteralCase{StorageType::kInt16, "-10.5", bytes::Bytes{0xff, 0xf5}}),
                         (
                             [](const auto& param_info)
                             {
                                 static constexpr std::array kNames{"PositiveHalf",       "NegativeHalf",
                                                                    "PositiveTwoAndHalf", "NegativeTwoAndHalf",
                                                                    "PositiveTenAndHalf", "NegativeTenAndHalf"};
                                 return kNames[param_info.index];
                             }));

INSTANTIATE_TEST_SUITE_P(FloatStorage, AssignedLiterals,
                         ::testing::Values(LiteralCase{StorageType::kFloat, "1.5", bytes::Bytes{0x3f, 0xc0, 0, 0}},
                                           LiteralCase{StorageType::kFloat, "1e100", std::nullopt},
                                           LiteralCase{StorageType::kFloat, "1/0", std::nullopt}),
                         (
                             [](const auto& param_info)
                             {
                                 static constexpr std::array kNames{"FiniteFloat", "FloatOverflow", "DivisionByZero"};
                                 return kNames[param_info.index];
                             }));

struct DefinitionLimitCase
{
    std::string_view minimum;
    std::string_view maximum;
    std::string_view formula;
    Encoding encoded;
};

class DefinitionLimits : public ::testing::TestWithParam<DefinitionLimitCase>
{
};

TEST_P(DefinitionLimits, ClampBeforeEncodingOrRejectBrokenLimits)
{
    const auto& example = GetParam();
    auto spec = SpecFor(StorageType::kUint8);
    spec.min_value = example.minimum;
    spec.max_value = example.maximum;
    const auto rom = Filler(1);
    const std::array<NumericCell, 1> cells{0.0};
    const auto result = CalculateAssignment(rom, spec, 1, cells, kOneCell, example.formula);
    if (!example.encoded.has_value())
    {
        EXPECT_THAT(result, IsErr(ErrorKind::kInvalidConfig));
        return;
    }
    EXPECT_THAT(result, IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, *example.encoded}))));
}

INSTANTIATE_TEST_SUITE_P(Bounds, DefinitionLimits,
                         ::testing::Values(DefinitionLimitCase{"0", "100", "50", bytes::Bytes{50}},
                                           DefinitionLimitCase{"0", "100", "120", bytes::Bytes{100}},
                                           DefinitionLimitCase{"0", "100", "100", bytes::Bytes{100}},
                                           DefinitionLimitCase{"10", "100", "-5", bytes::Bytes{10}},
                                           DefinitionLimitCase{"10", "100", "10", bytes::Bytes{10}},
                                           DefinitionLimitCase{"5", "5", "9", bytes::Bytes{5}},
                                           DefinitionLimitCase{"5", "5", "1", bytes::Bytes{5}},
                                           DefinitionLimitCase{"", "100", "120", bytes::Bytes{100}},
                                           DefinitionLimitCase{"  ", "100", "120", bytes::Bytes{100}},
                                           DefinitionLimitCase{"10", "", "120", bytes::Bytes{120}},
                                           DefinitionLimitCase{"10", " \t", "120", bytes::Bytes{120}},
                                           DefinitionLimitCase{"", "", "7", bytes::Bytes{7}},
                                           DefinitionLimitCase{"broken", "100", "1", std::nullopt},
                                           DefinitionLimitCase{"0", "broken", "1", std::nullopt},
                                           DefinitionLimitCase{"101", "100", "1", std::nullopt}),
                         (
                             [](const auto& param_info)
                             {
                                 static constexpr std::array kNames{
                                     "WithinBounds",      "ClampAbove",       "AtMaximum",         "ClampBelow",
                                     "AtMinimum",         "EqualBoundsAbove", "EqualBoundsBelow",  "MissingMinimum",
                                     "WhitespaceMinimum", "MissingMaximum",   "WhitespaceMaximum", "NoBounds",
                                     "InvalidMinimum",    "InvalidMaximum",   "ReversedBounds"};
                                 return kNames[param_info.index];
                             }));

struct NoChangeCase
{
    std::string_view max_value;
    std::string_view to_byte;
    std::string_view assignment; // empty: a fine increment is applied instead
    double increment;
    std::vector<double> cells;
    bytes::Bytes rom;
    NoChangeReason reason;
    std::size_t write_count;
};

class NoChangeReasons : public ::testing::TestWithParam<NoChangeCase>
{
};

TEST_P(NoChangeReasons, ReportsWhyNothingChanged)
{
    const auto& example = GetParam();
    auto spec = SpecFor(StorageType::kUint8);
    spec.max_value = example.max_value;
    spec.to_byte = example.to_byte;
    spec.fine_increment = example.increment;
    const std::vector<NumericCell> cells(example.cells.begin(), example.cells.end());
    const auto width = static_cast<std::uint32_t>(cells.size());
    const SelectionRange range{.first_row = 0, .first_col = 0, .last_row = 0, .last_col = static_cast<int>(width) - 1};
    const auto result = example.assignment.empty()
                            ? CalculateIncrement(example.rom, spec, width, cells, range, IncrementStep::kFineUp)
                            : CalculateAssignment(example.rom, spec, width, cells, range, example.assignment);
    ASSERT_THAT(result, IsOk());
    EXPECT_THAT(result->writes, ::testing::SizeIs(example.write_count));
    EXPECT_EQ(result->no_change, example.reason);
}

INSTANTIATE_TEST_SUITE_P(
    Reasons, NoChangeReasons,
    ::testing::Values(
        // Clamped to the definition maximum, which the ROM already holds.
        NoChangeCase{"100", "x", "", 1, {100}, {100}, NoChangeReason::kDefinitionLimit, 0},
        // Applied once: 10.01 encodes to the stored 100 and is not claimed as a limit.
        NoChangeCase{"", "x*10", "", 0.01, {10}, {100}, NoChangeReason::kBelowStorageResolution, 0},
        NoChangeCase{"", "x", "5", 0, {5}, {5}, NoChangeReason::kUnchanged, 0},
        NoChangeCase{"100", "x", "", 0.5, {100, 50}, {100, 51}, NoChangeReason::kMultipleCauses, 0},
        NoChangeCase{"100", "x", "", 1, {100, 100}, {100, 100}, NoChangeReason::kDefinitionLimit, 0},
        NoChangeCase{"", "x", "", 1, {5}, {5}, NoChangeReason::kNone, 1}),
    (
        [](const auto& param_info)
        {
            static constexpr std::array kNames{"DefinitionLimit", "StorageResolution",      "Unchanged",
                                               "MultipleCauses",  "EntireSelectionLimited", "ChangedValue"};
            return kNames[param_info.index];
        }));

struct IncrementCase
{
    StorageType storage;
    double cell;
    IncrementStep step;
    double fine;
    double coarse;
    Encoding encoded;
};

class IncrementSteps : public ::testing::TestWithParam<IncrementCase>
{
};

TEST_P(IncrementSteps, UsesTheSelectedSizeAndDirection)
{
    const auto& example = GetParam();
    auto spec = SpecFor(example.storage);
    spec.fine_increment = example.fine;
    spec.coarse_increment = example.coarse;
    const auto rom = Filler(1);
    const std::array<NumericCell, 1> cells{example.cell};
    const auto result = CalculateIncrement(rom, spec, 1, cells, kOneCell, example.step);
    if (!example.encoded.has_value())
    {
        EXPECT_THAT(result, IsErr(ErrorKind::kInvalidConfig));
        return;
    }
    EXPECT_THAT(result, IsOkAnd(WritesAre(ElementsAre(CellWrite{0, 0, *example.encoded}))));
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

INSTANTIATE_TEST_SUITE_P(
    Steps, IncrementSteps,
    ::testing::Values(IncrementCase{StorageType::kUint8, 50, IncrementStep::kFineUp, 1, 10, bytes::Bytes{51}},
                      IncrementCase{StorageType::kUint8, 50, IncrementStep::kFineDown, 1, 10, bytes::Bytes{49}},
                      IncrementCase{StorageType::kUint8, 50, IncrementStep::kCoarseUp, 1, 10, bytes::Bytes{60}},
                      IncrementCase{StorageType::kUint8, 50, IncrementStep::kCoarseDown, 1, 10, bytes::Bytes{40}},
                      // The unused increment may be unset or invalid.
                      IncrementCase{StorageType::kUint8, 50, IncrementStep::kCoarseUp, 0, 10, bytes::Bytes{60}},
                      IncrementCase{StorageType::kUint8, 50, IncrementStep::kFineUp, 1, 0, bytes::Bytes{51}},
                      IncrementCase{StorageType::kUint8, 50, IncrementStep::kFineDown, 1, kNaN, bytes::Bytes{49}},
                      IncrementCase{StorageType::kUint8, 50, IncrementStep::kFineUp, 0, 10, std::nullopt},
                      IncrementCase{StorageType::kUint8, 50, IncrementStep::kCoarseDown, 1, 0, std::nullopt},
                      IncrementCase{StorageType::kUint8, 50, IncrementStep::kFineDown, kNaN, 10, std::nullopt},
                      IncrementCase{StorageType::kUint8, 50, IncrementStep::kCoarseUp, 1, kInf, std::nullopt},
                      IncrementCase{StorageType::kUint8, 0, IncrementStep::kFineDown, 1, 10, std::nullopt},
                      IncrementCase{StorageType::kInt8, -1, IncrementStep::kFineUp, 2, 10, bytes::Bytes{1}},
                      IncrementCase{StorageType::kInt8, 0, IncrementStep::kFineDown, 1, 10, bytes::Bytes{0xff}}),
    (
        [](const auto& param_info)
        {
            static constexpr std::array kNames{"FineUp",
                                               "FineDown",
                                               "CoarseUp",
                                               "CoarseDown",
                                               "CoarseIgnoresZeroFine",
                                               "FineIgnoresZeroCoarse",
                                               "FineIgnoresNanCoarse",
                                               "ZeroFine",
                                               "ZeroCoarse",
                                               "NanFine",
                                               "InfiniteCoarse",
                                               "UnsignedUnderflow",
                                               "CrossSignedZero",
                                               "SignedNegativeOne"};
            return kNames[param_info.index];
        }));

struct SelectionCase
{
    std::uint32_t width;
    std::uint32_t height;
    SelectionRange range;
    bool valid;
};

class SelectionBounds : public ::testing::TestWithParam<SelectionCase>
{
};

TEST_P(SelectionBounds, AcceptsOnlySelectionsInsideTheRun)
{
    const auto& example = GetParam();
    const auto spec = SpecFor(StorageType::kUint8);
    const auto expect_outcome = [&](const Result<NumericEditResult>& result)
    {
        if (example.valid)
        {
            EXPECT_THAT(result, IsOk());
        }
        else
        {
            EXPECT_THAT(result, IsErr(ErrorKind::kInvalidConfig));
        }
    };

    const auto rom = Filler(4);
    const std::vector<std::vector<double>> values{{1}};
    expect_outcome(CalculatePaste(rom, spec, example.width, example.height, example.range, values));

    // The run itself is only materialized when it is small.
    const std::uint64_t count = std::uint64_t{example.width} * example.height;
    if (count <= 64)
    {
        const std::vector<NumericCell> cells(count, 0.0);
        const auto run_rom = Filler(count);
        expect_outcome(CalculateAssignment(run_rom, spec, example.width, cells, example.range, "1"));
    }
}

INSTANTIATE_TEST_SUITE_P(
    Rejected, SelectionBounds,
    ::testing::Values(SelectionCase{0, 1, {0, 0, 0, 0}, false}, SelectionCase{1, 0, {0, 0, 0, 0}, false},
                      SelectionCase{1, 1, {-1, 0, 0, 0}, false}, SelectionCase{1, 1, {0, -1, 0, 0}, false},
                      SelectionCase{1, 2, {1, 0, 0, 0}, false}, SelectionCase{2, 1, {0, 1, 0, 0}, false},
                      SelectionCase{1, 1, {0, 0, 0, 1}, false}, SelectionCase{1, 1, {0, 0, 1, 0}, false},
                      SelectionCase{1, 1, {1, 0, 1, 0}, false}, SelectionCase{2, 1, {1, 0, 1, 0}, false},
                      SelectionCase{2, 2, {0, 0, 1, 2}, false}, SelectionCase{2, 2, {0, 0, 2, 1}, false},
                      // One cell more than a 32-bit index can address.
                      SelectionCase{65537, 65536, {0, 0, 0, 0}, false}),
    (
        [](const auto& param_info)
        {
            static constexpr std::array kNames{"EmptyWidth",
                                               "EmptyHeight",
                                               "NegativeFirstRow",
                                               "NegativeFirstColumn",
                                               "ReversedRows",
                                               "ReversedColumns",
                                               "LastColumnOutsideRun",
                                               "LastRowOutsideRun",
                                               "FirstRowOutsideRun",
                                               "FirstRowOutsideSingleRowRun",
                                               "LastColumnOutsideTwoColumnRun",
                                               "LastRowOutsideTwoRowRun",
                                               "TooManyAddressableCells"};
            return kNames[param_info.index];
        }));

INSTANTIATE_TEST_SUITE_P(Accepted, SelectionBounds,
                         ::testing::Values(SelectionCase{1, 1, {0, 0, 0, 0}, true},
                                           SelectionCase{2, 2, {0, 0, 1, 1}, true},
                                           SelectionCase{2, 2, {1, 1, 1, 1}, true},
                                           // Exactly as many cells as a 32-bit index can address.
                                           SelectionCase{65537, 65535, {0, 0, 0, 0}, true}),
                         (
                             [](const auto& param_info)
                             {
                                 static constexpr std::array kNames{"SingleCell", "FullGrid", "BottomRightCell",
                                                                    "MaximumAddressableGrid"};
                                 return kNames[param_info.index];
                             }));

struct PasteCase
{
    std::uint32_t width;
    std::uint32_t height;
    SelectionRange range;
    std::vector<std::vector<double>> values;
    std::vector<CellWrite> writes;
};

class PasteClipping : public ::testing::TestWithParam<PasteCase>
{
};

TEST_P(PasteClipping, WritesOnlyWhatFitsInsideTheRun)
{
    const auto& example = GetParam();
    const auto rom = Filler(std::size_t{example.width} * example.height);
    EXPECT_THAT(
        CalculatePaste(rom, SpecFor(StorageType::kUint8), example.width, example.height, example.range, example.values),
        IsOkAnd(WritesAre(ElementsAreArray(example.writes))));
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, PasteClipping,
    ::testing::Values(
        // Rows and columns beyond the run are dropped.
        PasteCase{2,
                  2,
                  {0, 0, 0, 0},
                  {{1, 2, 3}, {4}, {5, 6}},
                  {CellWrite{0, 0, {1}}, CellWrite{1, 1, {2}}, CellWrite{2, 2, {4}}}},
        // The first row sets how many columns are pasted.
        PasteCase{2, 2, {0, 0, 0, 0}, {{1}, {2, 3}}, {CellWrite{0, 0, {1}}, CellWrite{2, 2, {2}}}},
        PasteCase{1, 3, {1, 0, 1, 0}, {{1}, {2}, {3}}, {CellWrite{1, 1, {1}}, CellWrite{2, 2, {2}}}},
        PasteCase{3, 1, {0, 1, 0, 1}, {{1, 2, 3}}, {CellWrite{1, 1, {1}}, CellWrite{2, 2, {2}}}},
        PasteCase{3,
                  3,
                  {1, 1, 1, 1},
                  {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}},
                  {CellWrite{4, 4, {1}}, CellWrite{5, 5, {2}}, CellWrite{7, 7, {4}}, CellWrite{8, 8, {5}}}},
        PasteCase{2, 2, {0, 0, 0, 0}, {}, {}}),
    (
        [](const auto& param_info)
        {
            static constexpr std::array kNames{"ClipExtraRowsAndColumns", "FirstRowControlsWidth", "VerticalClip",
                                               "HorizontalClip",          "OffsetRectangle",       "EmptyPaste"};
            return kNames[param_info.index];
        }));

class OffsetInterpolation : public ::testing::TestWithParam<std::tuple<InterpolationMode, int>>
{
  protected:
    static constexpr std::uint32_t kWidth = 4;

    // The 3x3 selection starts at (origin, origin) in a 4x4 run. Cells outside
    // it are valid decoys, so reading a wrong cell changes the result.
    void SetUp() override
    {
        origin_ = std::get<1>(GetParam());
        rom_ = bytes::Bytes(16, 99);
        cells_ = std::vector<NumericCell>(16, 99.0);
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                rom_[Index(row, col)] = 0;
                cells_[Index(row, col)] = Fail(ErrorKind::kInvalidConfig, "invalid interior");
            }
        }
    }

    std::size_t Index(int row, int col) const
    {
        const auto origin = static_cast<std::size_t>(origin_);
        return (origin + static_cast<std::size_t>(row)) * kWidth + origin + static_cast<std::size_t>(col);
    }

    SelectionRange Range() const
    {
        return {.first_row = origin_, .first_col = origin_, .last_row = origin_ + 2, .last_col = origin_ + 2};
    }

    void SetValid(int row, int col, std::uint8_t value)
    {
        rom_[Index(row, col)] = value;
        cells_[Index(row, col)] = static_cast<double>(value);
    }

    CellWrite WriteAt(int row, int col, std::uint8_t value) const
    {
        const auto index = static_cast<std::uint32_t>(Index(row, col));
        return {index, index, {value}};
    }

    // Sets the cells the mode reads and returns the writes it must produce.
    std::vector<CellWrite> Prepare(InterpolationMode mode)
    {
        std::vector<CellWrite> writes;
        switch (mode)
        {
        case InterpolationMode::kHorizontal:
            for (int row = 0; row < 3; ++row)
            {
                const auto start = static_cast<std::uint8_t>(10 * (row + 1));
                SetValid(row, 0, start);
                SetValid(row, 2, start + 20);
                writes.push_back(WriteAt(row, 1, start + 10));
            }
            break;
        case InterpolationMode::kVertical:
            for (int col = 0; col < 3; ++col)
            {
                const auto start = static_cast<std::uint8_t>(10 * (col + 1));
                SetValid(0, col, start);
                SetValid(2, col, start + 20);
            }
            writes = {WriteAt(1, 0, 20), WriteAt(1, 1, 30), WriteAt(1, 2, 40)};
            break;
        case InterpolationMode::kBidirectional:
            SetValid(0, 0, 0);
            SetValid(0, 2, 20);
            SetValid(2, 0, 40);
            SetValid(2, 2, 60);
            writes = {WriteAt(0, 1, 10), WriteAt(1, 0, 20), WriteAt(1, 1, 30), WriteAt(1, 2, 40), WriteAt(2, 1, 50)};
            break;
        }
        return writes;
    }

    int origin_{0};
    bytes::Bytes rom_;
    std::vector<NumericCell> cells_;
};

TEST_P(OffsetInterpolation, InterpolatesBetweenTheSelectionEdges)
{
    const auto mode = std::get<0>(GetParam());
    const auto expected = Prepare(mode);
    EXPECT_THAT(CalculateInterpolation(rom_, SpecFor(StorageType::kUint8), kWidth, cells_, Range(), mode),
                IsOkAnd(WritesAre(ElementsAreArray(expected))));
}

TEST_P(OffsetInterpolation, RejectsInvalidEndpoint)
{
    const auto mode = std::get<0>(GetParam());
    Prepare(mode);
    cells_[Index(0, 0)] = Fail(ErrorKind::kInvalidConfig, "invalid endpoint");
    EXPECT_THAT(CalculateInterpolation(rom_, SpecFor(StorageType::kUint8), kWidth, cells_, Range(), mode),
                IsErr(ErrorKind::kInvalidConfig));
}

INSTANTIATE_TEST_SUITE_P(ModesAndOrigins, OffsetInterpolation,
                         ::testing::Combine(::testing::Values(InterpolationMode::kHorizontal,
                                                              InterpolationMode::kVertical,
                                                              InterpolationMode::kBidirectional),
                                            ::testing::Values(0, 1)),
                         (
                             [](const auto& param_info)
                             {
                                 const auto mode = std::get<0>(param_info.param);
                                 const std::string name = mode == InterpolationMode::kHorizontal ? "Horizontal"
                                                          : mode == InterpolationMode::kVertical ? "Vertical"
                                                                                                 : "Bidirectional";
                                 return name + "Origin" + std::to_string(std::get<1>(param_info.param));
                             }));

struct CrossOperationCase
{
    StorageType storage;
    std::size_t width;
    double value;
    std::string_view maximum;
};

// Independent oracle: a big-endian two's-complement encoding of the clamped value.
bytes::Bytes ExpectedEncoding(const CrossOperationCase& example)
{
    auto target = static_cast<std::int64_t>(example.value);
    if (!example.maximum.empty())
    {
        target = std::min<std::int64_t>(target, 50);
    }
    bytes::Bytes encoded;
    for (std::size_t i = example.width; i-- > 0;)
    {
        encoded.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(target) >> (8 * i)) & 0xff));
    }
    return encoded;
}

std::vector<CrossOperationCase> CrossOperationCases()
{
    struct Storage
    {
        StorageType type;
        std::size_t width;
        bool is_signed;
    };
    const std::array<Storage, 8> storages{{{StorageType::kUint8, 1, false},
                                           {StorageType::kInt8, 1, true},
                                           {StorageType::kUint16, 2, false},
                                           {StorageType::kInt16, 2, true},
                                           {StorageType::kUint24, 3, false},
                                           {StorageType::kInt24, 3, true},
                                           {StorageType::kUint32, 4, false},
                                           {StorageType::kInt32, 4, true}}};
    std::vector<CrossOperationCase> cases;
    for (const auto& storage : storages)
    {
        for (const std::string_view maximum : {"", "50"})
        {
            for (const double value : {1.0, 100.0, -5.0})
            {
                if (value < 0 && !storage.is_signed)
                {
                    continue;
                }
                cases.push_back({storage.type, storage.width, value, maximum});
            }
        }
    }
    return cases;
}

class CrossOperationEncoding : public ::testing::TestWithParam<CrossOperationCase>
{
};

TEST_P(CrossOperationEncoding, EveryOperationWritesTheSameBytes)
{
    const auto& example = GetParam();
    auto spec = SpecFor(example.storage);
    spec.max_value = example.maximum;
    spec.fine_increment = 1;
    const auto encoded = ExpectedEncoding(example);
    const auto filler = Filler(example.width);
    const std::array<NumericCell, 1> before{example.value - 1};
    const std::array<NumericCell, 1> zero{0.0};
    const auto expected = ElementsAre(CellWrite{0, 0, encoded});

    EXPECT_THAT(CalculateAssignment(filler, spec, 1, zero, kOneCell, std::format("{}", example.value)),
                IsOkAnd(WritesAre(expected)));
    EXPECT_THAT(CalculatePaste(filler, spec, 1, 1, kOneCell, std::vector<std::vector<double>>{{example.value}}),
                IsOkAnd(WritesAre(expected)));
    EXPECT_THAT(CalculateIncrement(filler, spec, 1, before, kOneCell, IncrementStep::kFineUp),
                IsOkAnd(WritesAre(expected)));

    // Equal endpoints interpolate to the same value in the invalid middle cell.
    bytes::Bytes rom;
    for (const auto& part : {encoded, filler, encoded})
    {
        rom.insert(rom.end(), part.begin(), part.end());
    }
    const std::array<NumericCell, 3> row{example.value, Fail(ErrorKind::kInvalidConfig, "invalid"), example.value};
    EXPECT_THAT(CalculateInterpolation(rom, spec, 3, row, kThreeCells, InterpolationMode::kHorizontal),
                IsOkAnd(WritesAre(ElementsAre(CellWrite{1, example.width, encoded}))));
}

INSTANTIATE_TEST_SUITE_P(StoragesAndLimits, CrossOperationEncoding, ::testing::ValuesIn(CrossOperationCases()),
                         (
                             [](const auto& param_info)
                             {
                                 const auto& example = param_info.param;
                                 const std::string storage = [type = example.storage]
                                 {
                                     switch (type)
                                     {
                                     case StorageType::kUint8:
                                         return "Uint8";
                                     case StorageType::kInt8:
                                         return "Int8";
                                     case StorageType::kUint16:
                                         return "Uint16";
                                     case StorageType::kInt16:
                                         return "Int16";
                                     case StorageType::kUint24:
                                         return "Uint24";
                                     case StorageType::kInt24:
                                         return "Int24";
                                     case StorageType::kUint32:
                                         return "Uint32";
                                     case StorageType::kInt32:
                                         return "Int32";
                                     case StorageType::kFloat:
                                         return "Float";
                                     case StorageType::kBloblist:
                                         return "Bloblist";
                                     }
                                     return "Unknown";
                                 }();
                                 return storage + (example.maximum.empty() ? "Unbounded" : "Limited") +
                                        (example.value < 0 ? "Negative" : "Positive") +
                                        std::to_string(
                                            static_cast<int>(example.value < 0 ? -example.value : example.value));
                             }));

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

TEST(NumericMapEdit, UsesFullDoubleValueDespiteDisplayRounding)
{
    auto spec = SpecFor(StorageType::kUint16);
    spec.to_byte = "x*10000";
    const bytes::Bytes rom{0, 1};
    const std::array<NumericCell, 1> cells{0.0001};
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "x+0.01"),
                IsOkAnd(WritesAre(ElementsAre(Field(&CellWrite::bytes, ElementsAre(0, 101))))));
}

TEST(NumericMapEdit, RejectsFailingEncodingExpression)
{
    auto spec = SpecFor(StorageType::kUint8);
    spec.to_byte = "1/0";
    const bytes::Bytes rom{0};
    const std::array<NumericCell, 1> cells{0.0};
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "1"), IsErr(ErrorKind::kInvalidConfig));
}

TEST(NumericMapEdit, RejectsNonFinitePastedValueBeforeReturningAnyWrites)
{
    const auto spec = SpecFor(StorageType::kUint8);
    const bytes::Bytes rom{0, 0};
    const std::vector<std::vector<double>> pasted{{1, std::numeric_limits<double>::infinity()}};
    EXPECT_THAT(CalculatePaste(rom, spec, 2, 1, kOneCell, pasted), IsErr(ErrorKind::kInvalidConfig));
}

TEST(NumericMapEdit, RejectsAssignmentWithUnsupportedTokenBeforeReadingCells)
{
    const auto spec = SpecFor(StorageType::kUint8);
    const bytes::Bytes rom{0};
    const std::array<NumericCell, 1> cells{0.0};
    EXPECT_THAT(CalculateAssignment(rom, spec, 1, cells, kOneCell, "y"),
                IsErrWith(ErrorKind::kInvalidConfig, "assignment contains an unsupported token"));
}

TEST(NumericMapEdit, RejectsPasteWhenDefinitionLimitsAreBroken)
{
    auto spec = SpecFor(StorageType::kUint8);
    spec.min_value = "100";
    spec.max_value = "0";
    const bytes::Bytes rom{0};
    const std::vector<std::vector<double>> pasted{{1}};
    EXPECT_THAT(CalculatePaste(rom, spec, 1, 1, kOneCell, pasted),
                IsErrWith(ErrorKind::kInvalidConfig, "definition minimum exceeds maximum"));
}
} // namespace
} // namespace fastecu::calibration
