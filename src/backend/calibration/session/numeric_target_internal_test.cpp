#include "src/backend/calibration/session/numeric_target_internal.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/calibration/session/testing/numeric_session_fixture.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration::internal
{
namespace
{
using fastecu::testing::IsOkAnd;
using ::testing::Field;
using ::testing::VariantWith;

SelectionRange Cell(int row, int col)
{
    return {.first_row = row, .first_col = col, .last_row = row, .last_col = col};
}

class NumericTargetInternalTest : public calibration::testing::NumericSessionFixture
{
};

TEST_F(NumericTargetInternalTest, LocateMapReportsEachMiss)
{
    const auto reason = [](const std::variant<LocatedMap, MapNotApplicable>& located)
    {
        const auto *miss = std::get_if<MapNotApplicable>(&located);
        return miss != nullptr ? std::optional{miss->reason} : std::nullopt;
    };
    EXPECT_EQ(reason(LocateMap(workspace_, SessionId{9999}, 0)), NotApplicableReason::kClosedSession);
    EXPECT_EQ(reason(LocateMap(workspace_, id_, 5)), NotApplicableReason::kUnknownMap);
    EXPECT_EQ(reason(LocateMap(workspace_, id_, 0)), std::nullopt);
    Install(std::nullopt);
    EXPECT_EQ(reason(LocateMap(workspace_, id_, 0)), NotApplicableReason::kNoDefinition);
}

TEST_F(NumericTargetInternalTest, MovedPreparationKeepsMetadataAndCellsUsable)
{
    std::optional<PreparedNumericTarget> moved;
    {
        auto preparation = PrepareNumericTarget(workspace_, id_, 0, NumericTarget::kMapBody);
        ASSERT_THAT(preparation, VariantWith<PreparedNumericTarget>(::testing::_));
        moved.emplace(std::move(std::get<PreparedNumericTarget>(preparation)));
    }
    EXPECT_THAT(moved->Copy(Cell(0, 0)), IsOkAnd(VariantWith<NumericCopyText>(Field(&NumericCopyText::text, "10"))));
    EXPECT_THAT(moved->Apply(Cell(0, 0), AssignmentEdit{"7"}), IsOkAnd(VariantWith<NumericEditChanged>(::testing::_)));
    EXPECT_EQ(Session().File()[calibration::testing::kBody], 7);
}

TEST_F(NumericTargetInternalTest, MovedPreparationKeepsHeapBackedExpressionsUsable)
{
    auto def = calibration::testing::GridDefinition();
    def.scalings[0].from_byte = "x/3" + std::string(128, ' ');
    def.scalings[0].to_byte = "x*3" + std::string(128, ' ');
    Install(std::move(def));
    std::optional<PreparedNumericTarget> moved;
    {
        auto preparation = PrepareNumericTarget(workspace_, id_, 0, NumericTarget::kMapBody);
        ASSERT_THAT(preparation, VariantWith<PreparedNumericTarget>(::testing::_));
        moved.emplace(std::move(std::get<PreparedNumericTarget>(preparation)));
    }
    EXPECT_THAT(moved->Copy(Cell(0, 0)),
                IsOkAnd(VariantWith<NumericCopyText>(Field(&NumericCopyText::text, "3.3333333333333335"))));
    EXPECT_THAT(moved->Apply(Cell(0, 0), AssignmentEdit{"7"}), IsOkAnd(VariantWith<NumericEditChanged>(::testing::_)));
    EXPECT_EQ(Session().File()[calibration::testing::kBody], 21);
}

} // namespace
} // namespace fastecu::calibration::internal
