#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QMdiSubWindow>
#include <QString>
#include <QTableWidget>
#include <QTableWidgetSelectionRange>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "src/algorithms/memory/testing/memory_views.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::ui
{
namespace
{

// QMdiSubWindow/QTableWidget are QWidgets, which abort at construction
// without a live QApplication. This suite links fastecu_gtest's plain
// gtest_main, so bring one up via a ::testing::Environment, as other widget
// suites do. SetUp() runs after static
// initialization and after InitGoogleTest, and gtest tears the Environment
// down deterministically after all tests.
class MapEditAdapterEnvironment final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        static int argc = 1;
        static auto program = std::to_array("map_edit_adapter_test");
        static auto argv = std::to_array<char *>({program.data(), nullptr});
        app_ = std::make_unique<QApplication>(argc, argv.data());
    }

  private:
    std::unique_ptr<QApplication> app_;
};

const auto *const kMapEditAdapterEnvironment = ::testing::AddGlobalTestEnvironment(new MapEditAdapterEnvironment);

definition::RomDefinition twoByTwoDefinition()
{
    definition::RomDefinition def;
    definition::Scaling scaling;
    scaling.name = "body";
    scaling.from_byte = "x*2";
    scaling.coarse_increment = "1.0";
    scaling.fine_increment = "0.1";
    def.scalings.push_back(scaling);
    definition::CalibrationMap map;
    map.name = "Timing";
    map.address = memory::DefinitionAddress{16};
    map.storage_type = definition::StorageType::kUint16;
    map.endian = "big";
    map.scaling_name = "body";
    map.x_size = 2;
    map.y_size = 2;
    map.start_position = 2;
    map.interval = 3;
    map.x_axis.type = "X Axis";
    map.x_axis.address = memory::DefinitionAddress{64};
    map.x_axis.storage_type = definition::StorageType::kUint8;
    map.x_axis.from_byte = "x*10";
    map.x_axis.to_byte = "x/10";
    map.y_axis.type = "Y Axis";
    map.y_axis.address = memory::DefinitionAddress{80};
    map.y_axis.storage_type = definition::StorageType::kInt16;
    map.y_axis.endian = "big";
    map.y_axis.from_byte = "x/4";
    map.y_axis.to_byte = "x*4";
    def.maps.push_back(map);
    return def;
}

calibration::CalibrationSession sessionFrom(definition::RomDefinition def = twoByTwoDefinition())
{
    bytes::Bytes rom(128);
    for (unsigned i = 0; i < 4; ++i)
    {
        rom[19 + i * 6] = static_cast<std::uint8_t>(i + 1);
    }
    return calibration::CalibrationSession(
        calibration::SessionId{1},
        calibration::SessionContents{.image = memory::testing::IdentityImage(std::move(rom)),
                                     .definition = calibration::ResolvedDefinition{.definition = std::move(def)}});
}

TEST(ParseMapWindowId, ReturnsNulloptForANullWindow)
{
    EXPECT_FALSE(parseMapWindowId(nullptr).has_value());
}

TEST(ParseMapWindowId, ParsesSessionAndMapNumberFromAWellFormedObjectName)
{
    QMdiSubWindow window;
    window.setObjectName("12,7,Timing,uint16");

    const auto id = parseMapWindowId(&window);

    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->session, calibration::SessionId{12});
    EXPECT_EQ(id->map_number, 7);
}

TEST(ParseMapWindowId, ReturnsNulloptWhenTheSessionKeyIsNotDecimal)
{
    QMdiSubWindow window;
    window.setObjectName("x,7,Timing");

    EXPECT_FALSE(parseMapWindowId(&window).has_value());
}

// Legacy read mapWindowString.at(0)/.at(1)/.at(2)/.at(3) unguarded; this is
// the malformed-name case parse_map_window_id exists to guard against --
// fewer than the two leading fields it needs.
TEST(ParseMapWindowId, ReturnsNulloptForATooShortObjectName)
{
    QMdiSubWindow window;
    window.setObjectName("5");

    EXPECT_FALSE(parseMapWindowId(&window).has_value());
}

// Builds a map subwindow the way the legacy handlers found it: a
// QTableWidget child whose objectName() matches the subwindow's own, with
// row/column 0 reserved for axis headers (matching resolve_edit_target's
// widget-coordinate convention). Returns the table so callers can drive its
// selection.
QTableWidget *buildMapWindow(QMdiSubWindow& window, int rows, int cols)
{
    window.setObjectName("0,0,Timing,uint16");
    auto *table = new QTableWidget(rows, cols, &window);
    table->setObjectName(window.objectName());
    window.setWidget(table);
    return table;
}

TEST(SelectedNumericTarget, TranslatesBodyAndAxisHeaderSelections)
{
    QMdiSubWindow window;
    auto *table = buildMapWindow(window, 3, 3);
    const auto session = sessionFrom();
    const auto select = [table](int top, int left, int bottom, int right)
    {
        table->clearSelection();
        table->setRangeSelected(QTableWidgetSelectionRange(top, left, bottom, right), true);
    };

    select(1, 1, 2, 2);
    const auto body = selectedNumericTarget(&window, session, 0);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(body->target, calibration::NumericTarget::kMapBody);
    EXPECT_THAT(body->elements, ::testing::FieldsAre(0, 0, 1, 1));

    select(0, 1, 0, 2);
    const auto xAxis = selectedNumericTarget(&window, session, 0);
    ASSERT_TRUE(xAxis.has_value());
    EXPECT_EQ(xAxis->target, calibration::NumericTarget::kXAxis);
    EXPECT_THAT(xAxis->elements, ::testing::FieldsAre(0, 0, 0, 1));

    select(1, 0, 2, 0);
    const auto yAxis = selectedNumericTarget(&window, session, 0);
    ASSERT_TRUE(yAxis.has_value());
    EXPECT_EQ(yAxis->target, calibration::NumericTarget::kYAxis);
    EXPECT_THAT(yAxis->elements, ::testing::FieldsAre(0, 0, 1, 0));
}

TEST(BodyWidgetRange, SkipsTheAxisRowAndColumnOfAThreeDimensionalTable)
{
    const auto session = sessionFrom();

    const auto range = bodyWidgetRange(session, 0, 3, 3);

    ASSERT_TRUE(range.has_value());
    EXPECT_THAT(*range, ::testing::FieldsAre(1, 1, 2, 2));
}

TEST(BodyWidgetRange, SkipsOnlyTheAxisRowWhenTheMapHasNoYAxisColumn)
{
    auto def = twoByTwoDefinition();
    def.maps[0].y_size = 1;
    def.maps[0].x_size = 3;
    const auto session = sessionFrom(std::move(def));

    const auto range = bodyWidgetRange(session, 0, 2, 3);

    ASSERT_TRUE(range.has_value());
    EXPECT_THAT(*range, ::testing::FieldsAre(1, 0, 1, 2));
}

TEST(BodyWidgetRange, SkipsStaticAxisHeadersToo)
{
    auto def = twoByTwoDefinition();
    def.maps[0].x_axis.type = "Static X Axis";
    const auto session = sessionFrom(std::move(def));

    const auto range = bodyWidgetRange(session, 0, 3, 3);

    ASSERT_TRUE(range.has_value());
    EXPECT_THAT(*range, ::testing::FieldsAre(1, 1, 2, 2));
}

TEST(BodyWidgetRange, IsEmptyWithoutADefinitionAMapOrAnyCells)
{
    const auto session = sessionFrom();
    EXPECT_FALSE(bodyWidgetRange(session, 1, 3, 3).has_value());
    EXPECT_FALSE(bodyWidgetRange(session, -1, 3, 3).has_value());
    EXPECT_FALSE(bodyWidgetRange(session, 0, 0, 0).has_value());
    const calibration::CalibrationSession definitionless(
        calibration::SessionId{2}, calibration::SessionContents{.image = memory::testing::IdentityImage({0})});
    EXPECT_FALSE(bodyWidgetRange(definitionless, 0, 3, 3).has_value());
}

TEST(SelectedNumericTarget, IsEmptyWithoutANumericSelection)
{
    const auto session = sessionFrom();
    EXPECT_FALSE(selectedNumericTarget(nullptr, session, 0).has_value());

    QMdiSubWindow bare;
    bare.setObjectName("0,0,Timing,uint16");
    EXPECT_FALSE(selectedNumericTarget(&bare, session, 0).has_value());

    QMdiSubWindow window;
    auto *table = buildMapWindow(window, 3, 3);
    EXPECT_FALSE(selectedNumericTarget(&window, session, 0).has_value());

    table->setRangeSelected(QTableWidgetSelectionRange(1, 1, 1, 1), true);
    EXPECT_FALSE(selectedNumericTarget(&window, session, 1).has_value());
    EXPECT_FALSE(selectedNumericTarget(&window, session, -1).has_value());
    const calibration::CalibrationSession definitionless(
        calibration::SessionId{2}, calibration::SessionContents{.image = memory::testing::IdentityImage({0})});
    EXPECT_FALSE(selectedNumericTarget(&window, definitionless, 0).has_value());

    auto staticDef = twoByTwoDefinition();
    staticDef.maps[0].x_axis.type = "Static Y Axis";
    const auto staticSession = sessionFrom(std::move(staticDef));
    table->clearSelection();
    table->setRangeSelected(QTableWidgetSelectionRange(1, 0, 1, 0), true);
    EXPECT_FALSE(selectedNumericTarget(&window, staticSession, 0).has_value());
}

TEST(SplitPasteRows, SplitsTabSeparatedRowsAndDropsOneTerminalLf)
{
    using ::testing::ElementsAre;
    EXPECT_THAT(splitPasteRows("1\t2\n3\t4\n"), ElementsAre(ElementsAre("1", "2"), ElementsAre("3", "4")));
    EXPECT_THAT(splitPasteRows("a\nb\n\n"), ElementsAre(ElementsAre("a"), ElementsAre("b"), ElementsAre("")));
}

TEST(SplitPasteRows, PreservesEmptyCellsRaggedRowsAndCarriageReturns)
{
    using ::testing::ElementsAre;
    EXPECT_THAT(splitPasteRows("1\t\t3"), ElementsAre(ElementsAre("1", "", "3")));
    EXPECT_THAT(splitPasteRows("1\n\n2"), ElementsAre(ElementsAre("1"), ElementsAre(""), ElementsAre("2")));
    EXPECT_THAT(splitPasteRows("1\t2\n3"), ElementsAre(ElementsAre("1", "2"), ElementsAre("3")));
    EXPECT_THAT(splitPasteRows("20\r\n"), ElementsAre(ElementsAre("20\r")));
    EXPECT_THAT(splitPasteRows(""), ElementsAre(ElementsAre("")));
}

} // namespace
} // namespace fastecu::ui
