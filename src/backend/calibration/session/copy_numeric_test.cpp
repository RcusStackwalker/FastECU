#include <array>
#include "src/backend/calibration/session/map_operations.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/calibration/session/testing/numeric_session_fixture.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration
{
namespace
{

using testing::GridDefinition;
using testing::GridRom;
using testing::kBody;

using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::Field;
using ::testing::VariantWith;

SelectionRange Cell(int row, int col)
{
    return {.first_row = row, .first_col = col, .last_row = row, .last_col = col};
}

auto NotApplicable(NotApplicableReason reason)
{
    return IsOkAnd(VariantWith<MapNotApplicable>(Field(&MapNotApplicable::reason, reason)));
}

class CopyNumericTest : public testing::NumericSessionFixture
{
  protected:
    Result<NumericCopyOutcome> Copy(NumericTarget target, SelectionRange elements, std::size_t map_index = 0)
    {
        return CopyNumeric(workspace_, {id_, map_index}, {.target = target, .elements = elements});
    }
};

auto CopiedText(const std::string& text)
{
    return IsOkAnd(VariantWith<NumericCopyText>(Field(&NumericCopyText::text, text)));
}

TEST_F(CopyNumericTest, CopiesTheWholeBodyRowMajorWithTabsAndLineFeeds)
{
    EXPECT_THAT(Copy(NumericTarget::kMapBody, {.first_row = 0, .first_col = 0, .last_row = 1, .last_col = 2}),
                CopiedText("10\t20\t30\n40\t50\t200"));
}

TEST_F(CopyNumericTest, CopiesASubRangeFromItsTopLeft)
{
    EXPECT_THAT(Copy(NumericTarget::kMapBody, {.first_row = 0, .first_col = 1, .last_row = 1, .last_col = 2}),
                CopiedText("20\t30\n50\t200"));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(1, 0)), CopiedText("40"));
}

TEST_F(CopyNumericTest, CopiesAxesWithTheirOwnGeometry)
{
    EXPECT_THAT(Copy(NumericTarget::kXAxis, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 2}),
                CopiedText("1\t2\t3"));
    EXPECT_THAT(Copy(NumericTarget::kYAxis, {.first_row = 0, .first_col = 0, .last_row = 1, .last_col = 0}),
                CopiedText("4\n5"));
}

// The copied text is the scaled value, not what a display format would show.
TEST_F(CopyNumericTest, CopiesFullPrecisionScaledValuesNotRoundedDisplayText)
{
    auto def = GridDefinition();
    def.scalings[0].from_byte = "x/3";
    def.scalings[0].to_byte = "x*3";
    def.scalings[0].format = "%.1f";
    Install(std::move(def));

    const auto copied = Copy(NumericTarget::kMapBody, Cell(0, 0));

    ASSERT_THAT(copied, IsOk());
    const auto *text = std::get_if<NumericCopyText>(&*copied);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(std::strtod(text->text.c_str(), nullptr), 10.0 / 3.0);
    EXPECT_GT(text->text.size(), std::string("3.3").size());
}

TEST_F(CopyNumericTest, WritesPlainDecimalNeverExponentNotation)
{
    auto def = GridDefinition();
    def.scalings[0].from_byte = "x/1000000";
    def.scalings[0].to_byte = "x*1000000";
    Install(std::move(def));

    const auto copied = Copy(NumericTarget::kMapBody, Cell(0, 0));

    ASSERT_THAT(copied, IsOk());
    const auto *text = std::get_if<NumericCopyText>(&*copied);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text.find_first_of("eE"), std::string::npos);
    EXPECT_EQ(std::strtod(text->text.c_str(), nullptr), 10.0 / 1000000.0);
}

TEST_F(CopyNumericTest, PastingTheCopiedTextBackIsAnUnchangedEdit)
{
    auto def = GridDefinition();
    def.scalings[0].from_byte = "x/3";
    def.scalings[0].to_byte = "x*3";
    Install(std::move(def));
    const SelectionRange all{.first_row = 0, .first_col = 0, .last_row = 1, .last_col = 2};
    const auto copied = Copy(NumericTarget::kMapBody, all);
    ASSERT_THAT(copied, IsOk());
    const auto *text = std::get_if<NumericCopyText>(&*copied);
    ASSERT_NE(text, nullptr);
    PasteEdit paste;
    std::vector<std::string> row;
    std::string cell_text;
    for (const char c : text->text)
    {
        if (c == '\t' || c == '\n')
        {
            row.push_back(std::exchange(cell_text, {}));
            if (c == '\n')
            {
                paste.rows.push_back(std::exchange(row, {}));
            }
            continue;
        }
        cell_text += c;
    }
    row.push_back(cell_text);
    paste.rows.push_back(row);

    const auto pasted = EditNumeric(workspace_, {id_, 0}, {.target = NumericTarget::kMapBody, .elements = all}, paste);

    EXPECT_THAT(pasted, IsOkAnd(VariantWith<NumericEditUnchanged>(::testing::_)));
}

TEST_F(CopyNumericTest, ReportsTheFirstInvalidCellAndNoText)
{
    auto def = GridDefinition();
    def.scalings[0].from_byte = "1/0";
    Install(std::move(def));

    const auto copied = Copy(NumericTarget::kMapBody, {.first_row = 0, .first_col = 1, .last_row = 1, .last_col = 2});

    ASSERT_THAT(copied, IsOk());
    const auto *invalid = std::get_if<NumericCopyInvalidCell>(&*copied);
    ASSERT_NE(invalid, nullptr);
    EXPECT_EQ(invalid->row, 0);
    EXPECT_EQ(invalid->col, 1);
    EXPECT_FALSE(invalid->detail.empty());
}

TEST_F(CopyNumericTest, CopyNeverChangesTheSession)
{
    const auto before = RomBytes();

    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), IsOk());

    EXPECT_EQ(RomBytes(), before);
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(CopyNumericTest, IsNotApplicableWithoutAUsableTarget)
{
    EXPECT_THAT(
        CopyNumeric(workspace_, {SessionId{9999}, 0}, {.target = NumericTarget::kMapBody, .elements = Cell(0, 0)}),
        NotApplicable(NotApplicableReason::kClosedSession));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0), 5), NotApplicable(NotApplicableReason::kUnknownMap));

    auto static_axis = GridDefinition();
    static_axis.maps[0].x_axis = {.type = "Static X Axis", .static_data = {"a", "b", "c"}};
    static_axis.maps[0].y_axis = {};
    Install(std::move(static_axis));
    EXPECT_THAT(Copy(NumericTarget::kXAxis, Cell(0, 0)), NotApplicable(NotApplicableReason::kUnavailableTarget));
    EXPECT_THAT(Copy(NumericTarget::kYAxis, Cell(0, 0)), NotApplicable(NotApplicableReason::kUnavailableTarget));

    auto blob = GridDefinition();
    blob.maps[0].storage_type = definition::StorageType::kBloblist;
    blob.scalings[0].selections = {{.name = "one", .value = {1}}};
    Install(std::move(blob));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), NotApplicable(NotApplicableReason::kUnavailableTarget));
    EXPECT_EQ(RomBytes(), GridRom());
    EXPECT_FALSE(Session().Dirty());

    Install(std::nullopt);
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), NotApplicable(NotApplicableReason::kNoDefinition));
}

TEST_F(CopyNumericTest, UnknownMapIndexIsTheSameReasonForEveryOperation)
{
    const SelectionRange cell = Cell(0, 0);
    EXPECT_THAT(Copy(NumericTarget::kMapBody, cell, 5), NotApplicable(NotApplicableReason::kUnknownMap));
    EXPECT_THAT(
        EditNumeric(workspace_, {id_, 5}, {.target = NumericTarget::kMapBody, .elements = cell}, IncrementEdit{}),
        NotApplicable(NotApplicableReason::kUnknownMap));
    EXPECT_THAT(
        EditSelectable(workspace_, {id_, 5}, "any"),
        IsOkAnd(VariantWith<MapNotApplicable>(Field(&MapNotApplicable::reason, NotApplicableReason::kUnknownMap))));
}

TEST_F(CopyNumericTest, CopyReadsValuesAfterAnEdit)
{
    ASSERT_THAT(EditNumeric(workspace_, {id_, 0}, {.target = NumericTarget::kMapBody, .elements = Cell(0, 0)},
                            AssignmentEdit{"33"}),
                IsOkAnd(VariantWith<NumericEditChanged>(::testing::_)));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), CopiedText("33"));
}

TEST_F(CopyNumericTest, AnInvalidAxisRejectsBodyOperations)
{
    auto def = GridDefinition();
    def.maps[0].x_axis.address = memory::DefinitionAddress{0x40};
    Install(std::move(def));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), NotApplicable(NotApplicableReason::kUnavailableTarget));
    EXPECT_EQ(RomBytes(), GridRom());
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(CopyNumericTest, CopiesReciprocalScaledValues)
{
    auto def = GridDefinition();
    auto& map = def.maps[0];
    map.type = "1D";
    map.x_size = 1;
    map.y_size = 1;
    map.storage_type = definition::StorageType::kUint16;
    map.x_axis = {};
    map.y_axis = {};
    auto& scaling = def.scalings[0];
    scaling.from_byte = "7500000/x";
    scaling.to_byte = "7500000/x";
    scaling.minimum = "0";
    scaling.maximum = "9000";
    scaling.coarse_increment = "60";
    auto rom = GridRom();
    rom[kBody] = 0x03;
    rom[kBody + 1] = 0xE8;
    Install(std::move(def), rom);
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), CopiedText("7500"));
    EXPECT_EQ(RomBytes(), rom);
    EXPECT_FALSE(Session().Dirty());
}

class CopyIgnoresEditOnlyMetadataTest : public CopyNumericTest, public ::testing::WithParamInterface<int>
{
};

TEST_P(CopyIgnoresEditOnlyMetadataTest, CopyIgnoresEditOnlyMetadata)
{
    auto def = GridDefinition();
    auto& scaling = def.scalings[0];
    switch (GetParam())
    {
    case 0:
        scaling.to_byte = "1/0";
        break;
    case 1:
        scaling.minimum = "bad";
        break;
    case 2:
        scaling.minimum = "200";
        scaling.maximum = "0";
        break;
    case 3:
        scaling.fine_increment = "bad";
        break;
    default:
        FAIL() << "Unexpected edit metadata parameter: " << GetParam();
    }
    Install(std::move(def));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), CopiedText("10"));
    EXPECT_EQ(RomBytes(), GridRom());
    EXPECT_FALSE(Session().Dirty());
}

INSTANTIATE_TEST_SUITE_P(EditMetadata, CopyIgnoresEditOnlyMetadataTest, ::testing::Values(0, 1, 2, 3),
                         (
                             [](const auto& info)
                             {
                                 static constexpr std::array kNames{"InvalidToByte", "InvalidMinimum", "ReversedBounds",
                                                                    "InvalidIncrement"};
                                 return kNames[info.index];
                             }));

TEST_F(CopyNumericTest, ClosingAnotherSessionKeepsTheTargetIdStable)
{
    const auto other =
        workspace_.AdoptReadImage(ReadImage{.rom = GridRom(), .filename = "other.bin", .protocol_name = "proto_b"});
    ASSERT_THAT(other, IsOk());
    ASSERT_THAT(workspace_.Close(other->id), IsOk());
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), CopiedText("10"));
    ASSERT_THAT(EditNumeric(workspace_, {id_, 0}, {.target = NumericTarget::kMapBody, .elements = Cell(0, 0)},
                            AssignmentEdit{"7"}),
                IsOkAnd(VariantWith<NumericEditChanged>(::testing::_)));
    EXPECT_EQ(Session().File()[kBody], 7);
    ASSERT_THAT(workspace_.Close(id_), IsOk());
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), NotApplicable(NotApplicableReason::kClosedSession));
    EXPECT_THAT(EditNumeric(workspace_, {id_, 0}, {.target = NumericTarget::kMapBody, .elements = Cell(0, 0)},
                            AssignmentEdit{"7"}),
                NotApplicable(NotApplicableReason::kClosedSession));
}

TEST_F(CopyNumericTest, RejectsSelectionsOutsideTheRun)
{
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(2, 0)), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 3)), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(-1, 0)), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, {.first_row = 1, .first_col = 0, .last_row = 0, .last_col = 0}),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(Copy(NumericTarget::kXAxis, Cell(1, 0)), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(Copy(NumericTarget::kYAxis, Cell(0, 1)), IsErr(ErrorKind::kInvalidConfig));
}

} // namespace
} // namespace fastecu::calibration
