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

const auto *map_edit_adapter_environment = ::testing::AddGlobalTestEnvironment(new MapEditAdapterEnvironment);

definition::RomDefinition two_by_two_definition()
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
    map.address = 16;
    map.storage_type = definition::StorageType::Uint16;
    map.endian = "big";
    map.scaling_name = "body";
    map.x_size = 2;
    map.y_size = 2;
    map.start_position = 2;
    map.interval = 3;
    map.x_axis.type = "X Axis";
    map.x_axis.address = 64;
    map.x_axis.storage_type = definition::StorageType::Uint8;
    map.x_axis.from_byte = "x*10";
    map.x_axis.to_byte = "x/10";
    map.y_axis.type = "Y Axis";
    map.y_axis.address = 80;
    map.y_axis.storage_type = definition::StorageType::Int16;
    map.y_axis.endian = "big";
    map.y_axis.from_byte = "x/4";
    map.y_axis.to_byte = "x*4";
    def.maps.push_back(map);
    return def;
}

calibration::CalibrationSession session_from(definition::RomDefinition def = two_by_two_definition())
{
    calibration::SessionContents contents;
    contents.rom.resize(128);
    for (unsigned i = 0; i < 4; ++i)
    {
        contents.rom[19 + i * 6] = static_cast<std::uint8_t>(i + 1);
    }
    contents.definition = calibration::ResolvedDefinition{.definition = std::move(def)};
    contents.protocol.flash_method = "wrx02";
    contents.protocol.unpadded_size = 123;
    return calibration::CalibrationSession(calibration::SessionId{1}, std::move(contents));
}

TEST(ParseMapWindowId, ReturnsNulloptForANullWindow)
{
    EXPECT_FALSE(parse_map_window_id(nullptr).has_value());
}

TEST(ParseMapWindowId, ParsesSessionAndMapNumberFromAWellFormedObjectName)
{
    QMdiSubWindow window;
    window.setObjectName("12,7,Timing,uint16");

    const auto id = parse_map_window_id(&window);

    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->session, calibration::SessionId{12});
    EXPECT_EQ(id->map_number, 7);
}

TEST(ParseMapWindowId, ReturnsNulloptWhenTheSessionKeyIsNotDecimal)
{
    QMdiSubWindow window;
    window.setObjectName("x,7,Timing");

    EXPECT_FALSE(parse_map_window_id(&window).has_value());
}

// Legacy read mapWindowString.at(0)/.at(1)/.at(2)/.at(3) unguarded; this is
// the malformed-name case parse_map_window_id exists to guard against --
// fewer than the two leading fields it needs.
TEST(ParseMapWindowId, ReturnsNulloptForATooShortObjectName)
{
    QMdiSubWindow window;
    window.setObjectName("5");

    EXPECT_FALSE(parse_map_window_id(&window).has_value());
}

// Builds a map subwindow the way the legacy handlers found it: a
// QTableWidget child whose objectName() matches the subwindow's own, with
// row/column 0 reserved for axis headers (matching resolve_edit_target's
// widget-coordinate convention). Returns the table so callers can drive its
// selection.
QTableWidget *build_map_window(QMdiSubWindow& window, int rows, int cols)
{
    window.setObjectName("0,0,Timing,uint16");
    auto *table = new QTableWidget(rows, cols, &window);
    table->setObjectName(window.objectName());
    window.setWidget(table);
    return table;
}

TEST(ResolveActiveMapEdit, ReturnsNulloptForANullWindow)
{
    EXPECT_FALSE(resolve_active_map_edit(nullptr, session_from(), 0).has_value());
}

TEST(ResolveActiveMapEdit, ReturnsNulloptWhenNoMatchingTableWidgetIsFound)
{
    QMdiSubWindow window;
    window.setObjectName("0,0,Timing,uint16");
    // Deliberately no QTableWidget child added.

    EXPECT_FALSE(resolve_active_map_edit(&window, session_from(), 0).has_value());
}

TEST(ResolveActiveMapEdit, ReturnsNulloptWhenTheSelectionIsEmpty)
{
    QMdiSubWindow window;
    build_map_window(window, 3, 3);

    EXPECT_FALSE(resolve_active_map_edit(&window, session_from(), 0).has_value());
}

TEST(ResolveActiveMapEdit, ResolvesAMapBodySelectionToItsSpecRangeAndNumericCells)
{
    QMdiSubWindow window;
    auto *table = build_map_window(window, 3, 3);
    // Widget row/col 0 are axis headers; (1, 1) is the top-left data cell.
    table->setRangeSelected(QTableWidgetSelectionRange(1, 1, 1, 1), true);

    const auto def = session_from();
    const auto edit = resolve_active_map_edit(&window, def, 0);

    ASSERT_TRUE(edit.has_value());
    EXPECT_EQ(edit->kind(), calibration::EditTargetKind::MapBody);
    EXPECT_EQ(edit->map_number(), 0);
    EXPECT_EQ(edit->x_size(), 2U);
    EXPECT_EQ(edit->range().first_row, 0);
    EXPECT_EQ(edit->range().first_col, 0);
    EXPECT_EQ(edit->range().last_row, 0);
    EXPECT_EQ(edit->range().last_col, 0);

    ASSERT_EQ(edit->cells().size(), 4U);
    EXPECT_THAT(edit->cells()[0], fastecu::testing::IsOkAnd(2));
    EXPECT_THAT(edit->cells()[3], fastecu::testing::IsOkAnd(8));

    const auto spec = edit->spec();
    EXPECT_EQ(spec.address, 16U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::Uint16);
}

TEST(ResolveActiveMapEdit, OwnsDecodedSnapshotAcrossSessionEdits)
{
    QMdiSubWindow window;
    auto *table = build_map_window(window, 3, 3);
    table->setRangeSelected(QTableWidgetSelectionRange(1, 1, 1, 1), true);
    auto session = session_from();
    const auto edit = resolve_active_map_edit(&window, session, 0);
    ASSERT_TRUE(edit.has_value());
    const calibration::NumericEditPatch patch{{.index = 0, .byte_address = 18, .bytes = {0, 7}}};
    ASSERT_TRUE(apply_patch(session, 0, calibration::EditTargetKind::MapBody, patch).has_value());
    EXPECT_THAT(edit->cells()[0], fastecu::testing::IsOkAnd(2));
    const auto refreshed = resolve_active_map_edit(&window, session, 0);
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_THAT(refreshed->cells()[0], fastecu::testing::IsOkAnd(14));
}

TEST(ResolveActiveMapEdit, ReturnsNulloptForAStaticAxisSelection)
{
    QMdiSubWindow window;
    auto *table = build_map_window(window, 3, 3);
    // Column 0 with a multi-row map targets the Y axis, which is rejected
    // when the definition marks it static.
    table->setRangeSelected(QTableWidgetSelectionRange(1, 0, 1, 0), true);

    auto definition = two_by_two_definition();
    definition.maps[0].x_axis.type = "Static Y Axis";
    auto def = session_from(std::move(definition));

    EXPECT_FALSE(resolve_active_map_edit(&window, def, 0).has_value());
}

TEST(ApplyPatch, RejectsInvalidCellIndex)
{
    auto session = session_from();
    const auto before = std::vector<std::uint8_t>(session.rom().begin(), session.rom().end());
    calibration::NumericEditPatch patch{{.index = static_cast<std::uint32_t>(-1), .byte_address = 64, .bytes = {1, 2}}};
    EXPECT_THAT(apply_patch(session, 0, calibration::EditTargetKind::XAxis, patch),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(std::vector<std::uint8_t>(session.rom().begin(), session.rom().end()), before);
    EXPECT_FALSE(session.dirty());
}

TEST(ApplyPatch, RejectsIndexPastActualExtent)
{
    auto session = session_from();
    const calibration::NumericEditPatch patch{{.index = 4, .byte_address = 0, .bytes = {9}}};
    EXPECT_THAT(apply_patch(session, 0, calibration::EditTargetKind::MapBody, patch),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(session.rom()[0], 0);
    EXPECT_FALSE(session.dirty());
}

TEST(ApplyPatch, RejectsAddressOutsideSelectedTarget)
{
    auto session = session_from();
    const calibration::NumericEditPatch patch{{.index = 0, .byte_address = 0, .bytes = {0, 9}}};
    EXPECT_THAT(apply_patch(session, 0, calibration::EditTargetKind::MapBody, patch),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_FALSE(session.dirty());
}

TEST(ApplyPatch, NoOpPreservesCleanState)
{
    auto session = session_from();
    const calibration::NumericEditPatch patch{{.index = 0, .byte_address = 18, .bytes = {0, 1}}};
    ASSERT_THAT(apply_patch(session, 0, calibration::EditTargetKind::MapBody, patch), fastecu::testing::IsOk());
    EXPECT_FALSE(session.dirty());
}

TEST(ApplyPatch, FailedPatchPreservesAlreadyDirtyStateAndRejectsOverflow)
{
    auto session = session_from();
    const std::array<std::uint8_t, 1> initial{9};
    ASSERT_TRUE(session.write_bytes(0, initial).has_value());
    const auto before = std::vector<std::uint8_t>(session.rom().begin(), session.rom().end());
    const calibration::NumericEditPatch patch{{.index = 0, .byte_address = UINT64_MAX, .bytes = {1}}};
    EXPECT_FALSE(apply_patch(session, 0, calibration::EditTargetKind::MapBody, patch).has_value());
    EXPECT_EQ(std::vector<std::uint8_t>(session.rom().begin(), session.rom().end()), before);
    EXPECT_TRUE(session.dirty());
}

TEST(ApplyPatch, ValidatesAllRangesBeforeWriting)
{
    auto session = session_from();
    const auto before = std::vector<std::uint8_t>(session.rom().begin(), session.rom().end());
    calibration::NumericEditPatch patch{{.index = 0, .byte_address = 18, .bytes = {0, 7}},
                                        {.index = 1, .byte_address = 127, .bytes = {0, 8}}};
    EXPECT_FALSE(apply_patch(session, 0, calibration::EditTargetKind::MapBody, patch).has_value());
    EXPECT_EQ(std::vector<std::uint8_t>(session.rom().begin(), session.rom().end()), before);
    EXPECT_FALSE(session.dirty());
}

TEST(ApplyPatch, WritesBodyAndAxesAndDecodesCurrentBytes)
{
    auto session = session_from();
    auto expected = std::vector<std::uint8_t>(session.rom().begin(), session.rom().end());
    expected[19] = 7;
    expected[64] = 3;
    expected[83] = 12;
    const calibration::NumericEditPatch body{{.index = 0, .byte_address = 18, .bytes = {0, 7}}};
    const calibration::NumericEditPatch x{{.index = 0, .byte_address = 64, .bytes = {3}}};
    const calibration::NumericEditPatch y{{.index = 1, .byte_address = 82, .bytes = {0, 12}}};
    ASSERT_TRUE(apply_patch(session, 0, calibration::EditTargetKind::MapBody, body).has_value());
    ASSERT_TRUE(apply_patch(session, 0, calibration::EditTargetKind::XAxis, x).has_value());
    ASSERT_TRUE(apply_patch(session, 0, calibration::EditTargetKind::YAxis, y).has_value());
    EXPECT_EQ(std::vector<std::uint8_t>(session.rom().begin(), session.rom().end()), expected);
    EXPECT_EQ(session.rom()[18], 0);
    EXPECT_EQ(session.rom()[19], 7);
    EXPECT_EQ(session.rom()[64], 3);
    EXPECT_EQ(session.rom()[82], 0);
    EXPECT_EQ(session.rom()[83], 12);
    const auto decoded = session.decode_map(0);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_THAT(std::get<calibration::NumericRun>(decoded->body).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(14), fastecu::testing::IsOkAnd(4),
                                       fastecu::testing::IsOkAnd(6), fastecu::testing::IsOkAnd(8)));
    EXPECT_THAT(std::get<calibration::NumericRun>(decoded->x_axis).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(30), fastecu::testing::IsOkAnd(0)));
    EXPECT_THAT(std::get<calibration::NumericRun>(decoded->y_axis).cells,
                ::testing::ElementsAre(fastecu::testing::IsOkAnd(0), fastecu::testing::IsOkAnd(3)));
    EXPECT_TRUE(session.dirty());
}

TEST(SelectedNumericTarget, TranslatesBodyAndAxisHeaderSelections)
{
    QMdiSubWindow window;
    auto *table = build_map_window(window, 3, 3);
    const auto session = session_from();
    const auto select = [table](int top, int left, int bottom, int right)
    {
        table->clearSelection();
        table->setRangeSelected(QTableWidgetSelectionRange(top, left, bottom, right), true);
    };

    select(1, 1, 2, 2);
    const auto body = selected_numeric_target(&window, session, 0);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(body->target, calibration::NumericTarget::MapBody);
    EXPECT_THAT(body->elements, ::testing::FieldsAre(0, 0, 1, 1));

    select(0, 1, 0, 2);
    const auto x_axis = selected_numeric_target(&window, session, 0);
    ASSERT_TRUE(x_axis.has_value());
    EXPECT_EQ(x_axis->target, calibration::NumericTarget::XAxis);
    EXPECT_THAT(x_axis->elements, ::testing::FieldsAre(0, 0, 0, 1));

    select(1, 0, 2, 0);
    const auto y_axis = selected_numeric_target(&window, session, 0);
    ASSERT_TRUE(y_axis.has_value());
    EXPECT_EQ(y_axis->target, calibration::NumericTarget::YAxis);
    EXPECT_THAT(y_axis->elements, ::testing::FieldsAre(0, 0, 1, 0));
}

TEST(SelectedNumericTarget, IsEmptyWithoutANumericSelection)
{
    const auto session = session_from();
    EXPECT_FALSE(selected_numeric_target(nullptr, session, 0).has_value());

    QMdiSubWindow bare;
    bare.setObjectName("0,0,Timing,uint16");
    EXPECT_FALSE(selected_numeric_target(&bare, session, 0).has_value());

    QMdiSubWindow window;
    auto *table = build_map_window(window, 3, 3);
    EXPECT_FALSE(selected_numeric_target(&window, session, 0).has_value());

    table->setRangeSelected(QTableWidgetSelectionRange(1, 1, 1, 1), true);
    EXPECT_FALSE(selected_numeric_target(&window, session, 1).has_value());
    EXPECT_FALSE(selected_numeric_target(&window, session, -1).has_value());
    const calibration::CalibrationSession definitionless(calibration::SessionId{2}, calibration::SessionContents{});
    EXPECT_FALSE(selected_numeric_target(&window, definitionless, 0).has_value());

    auto static_def = two_by_two_definition();
    static_def.maps[0].x_axis.type = "Static Y Axis";
    const auto static_session = session_from(std::move(static_def));
    table->clearSelection();
    table->setRangeSelected(QTableWidgetSelectionRange(1, 0, 1, 0), true);
    EXPECT_FALSE(selected_numeric_target(&window, static_session, 0).has_value());
}

TEST(SplitPasteRows, SplitsTabSeparatedRowsAndDropsOneTerminalLf)
{
    using ::testing::ElementsAre;
    EXPECT_THAT(split_paste_rows("1\t2\n3\t4\n"), ElementsAre(ElementsAre("1", "2"), ElementsAre("3", "4")));
    EXPECT_THAT(split_paste_rows("a\nb\n\n"), ElementsAre(ElementsAre("a"), ElementsAre("b"), ElementsAre("")));
}

TEST(SplitPasteRows, PreservesEmptyCellsRaggedRowsAndCarriageReturns)
{
    using ::testing::ElementsAre;
    EXPECT_THAT(split_paste_rows("1\t\t3"), ElementsAre(ElementsAre("1", "", "3")));
    EXPECT_THAT(split_paste_rows("1\n\n2"), ElementsAre(ElementsAre("1"), ElementsAre(""), ElementsAre("2")));
    EXPECT_THAT(split_paste_rows("1\t2\n3"), ElementsAre(ElementsAre("1", "2"), ElementsAre("3")));
    EXPECT_THAT(split_paste_rows("20\r\n"), ElementsAre(ElementsAre("20\r")));
    EXPECT_THAT(split_paste_rows(""), ElementsAre(ElementsAre("")));
}

} // namespace
} // namespace fastecu::ui
