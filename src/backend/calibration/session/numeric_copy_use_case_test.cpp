#include "src/backend/calibration/session/numeric_copy_use_case.h"

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
    return IsOkAnd(VariantWith<NumericEditNotApplicable>(Field(&NumericEditNotApplicable::reason, reason)));
}

class NumericCopyUseCaseTest : public testing::NumericSessionFixture
{
  protected:
    Result<NumericCopyOutcome> Copy(NumericTarget target, SelectionRange elements, std::size_t map_index = 0)
    {
        return CopyNumericValues(
            workspace_,
            {.session = id_, .map_index = map_index, .selection = {.target = target, .elements = elements}});
    }
};

auto CopiedText(const std::string& text)
{
    return IsOkAnd(VariantWith<NumericCopyText>(Field(&NumericCopyText::text, text)));
}

TEST_F(NumericCopyUseCaseTest, CopiesTheWholeBodyRowMajorWithTabsAndLineFeeds)
{
    EXPECT_THAT(Copy(NumericTarget::kMapBody, {.first_row = 0, .first_col = 0, .last_row = 1, .last_col = 2}),
                CopiedText("10\t20\t30\n40\t50\t200"));
}

TEST_F(NumericCopyUseCaseTest, CopiesASubRangeFromItsTopLeft)
{
    EXPECT_THAT(Copy(NumericTarget::kMapBody, {.first_row = 0, .first_col = 1, .last_row = 1, .last_col = 2}),
                CopiedText("20\t30\n50\t200"));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(1, 0)), CopiedText("40"));
}

TEST_F(NumericCopyUseCaseTest, CopiesAxesWithTheirOwnGeometry)
{
    EXPECT_THAT(Copy(NumericTarget::kXAxis, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 2}),
                CopiedText("1\t2\t3"));
    EXPECT_THAT(Copy(NumericTarget::kYAxis, {.first_row = 0, .first_col = 0, .last_row = 1, .last_col = 0}),
                CopiedText("4\n5"));
}

// The copied text is the scaled value, not what a display format would show.
TEST_F(NumericCopyUseCaseTest, CopiesFullPrecisionScaledValuesNotRoundedDisplayText)
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

TEST_F(NumericCopyUseCaseTest, WritesPlainDecimalNeverExponentNotation)
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

TEST_F(NumericCopyUseCaseTest, PastingTheCopiedTextBackIsAnUnchangedEdit)
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

    const auto pasted = ApplyNumericEdit(workspace_, {.session = id_,
                                                      .map_index = 0,
                                                      .selection = {.target = NumericTarget::kMapBody, .elements = all},
                                                      .operation = paste});

    EXPECT_THAT(pasted, IsOkAnd(VariantWith<NumericEditUnchanged>(::testing::_)));
}

TEST_F(NumericCopyUseCaseTest, ReportsTheFirstInvalidCellAndNoText)
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

TEST_F(NumericCopyUseCaseTest, CopyNeverChangesTheSession)
{
    const auto before = RomBytes();

    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), IsOk());

    EXPECT_EQ(RomBytes(), before);
    EXPECT_FALSE(Session().Dirty());
}

TEST_F(NumericCopyUseCaseTest, IsNotApplicableWithoutAUsableTarget)
{
    EXPECT_THAT(
        CopyNumericValues(workspace_, {.session = SessionId{9999},
                                       .selection = {.target = NumericTarget::kMapBody, .elements = Cell(0, 0)}}),
        NotApplicable(NotApplicableReason::kClosedSession));
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0), 5), NotApplicable(NotApplicableReason::kUnavailableTarget));

    Install(std::nullopt);
    EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0, 0)), NotApplicable(NotApplicableReason::kNoDefinition));
}

TEST_F(NumericCopyUseCaseTest, RejectsSelectionsOutsideTheRun)
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
