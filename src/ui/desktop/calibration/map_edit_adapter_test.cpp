#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <array>
#include <bit>
#include <memory>
#include <string>

#include <QApplication>
#include <QMdiSubWindow>
#include <QString>
#include <QTableWidget>
#include <QTableWidgetSelectionRange>

#include <gtest/gtest.h>

namespace fastecu::ui
{
namespace
{

// QMdiSubWindow/QTableWidget are QWidgets, which abort at construction
// without a live QApplication. This suite links fastecu_gtest's plain
// gtest_main (map_edit_adapter.h declares no Q_OBJECT, so fastecu_qttest's
// QTEST_MAIN generator doesn't apply), so bring one up via a
// ::testing::Environment, mirroring MenuBuilderEnvironment in
// src/ui/desktop/menu/menu_builder_test.cpp. SetUp() runs after static
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

// MapElementFields::spec() is ref-qualified (`const &`, with `const && =
// delete`) so that `collect_map_element_fields(...).spec()` -- taking a spec
// from a temporary that is gone by the semicolon -- is a compile error
// instead of a dangle: that guarantee no longer needs a runtime test to
// observe it (a dangling implementation would very likely still pass a test
// that merely keeps `fields` alive in the same scope, since freed
// short-string storage usually reads back fine). A `static_assert` pinning
// this directly was tried and dropped: `requires { collect_map_element_
// fields(...).spec(); }` does not detect it -- calling a function selected
// by overload resolution but marked `= delete` is a hard compile error, not
// a substitution failure, so it is not swallowed by a requires-expression
// (confirmed by trying it: the whole translation unit fails to compile
// rather than the static_assert firing). Enforcement lives entirely in the
// ref-qualification itself; see PlucksMapBodyFields below for the ordinary,
// non-dangling usage this class expects from every caller.

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

TEST(MapEditAdapter, PlucksTypedFieldsAndUnpaddedProtocolSize)
{
    const auto session = session_from();
    const auto fields = collect_map_element_fields(session, 0, calibration::EditTargetKind::MapBody);
    const auto spec = fields.spec();
    EXPECT_EQ(spec.address, 16U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::Uint16);
    EXPECT_EQ(spec.from_byte, "x*2");
    EXPECT_EQ(spec.to_byte, "x");
    EXPECT_EQ(spec.min_value, " ");
    EXPECT_DOUBLE_EQ(spec.fine_increment, 0.1);
    EXPECT_EQ(spec.start_position, 2U);
    EXPECT_EQ(spec.interval, 3U);
    EXPECT_EQ(spec.flash_method, "wrx02");
    EXPECT_EQ(spec.rom_file_size, 123U);
}

TEST(MapEditAdapter, AxisUsesResolvedFieldsAndScalingBounds)
{
    auto def = two_by_two_definition();
    def.maps[0].x_axis.scaling_name = "body";
    def.maps[0].x_axis.start_position = 4;
    def.maps[0].x_axis.interval = 5;
    const auto session = session_from(std::move(def));
    const auto fields = collect_map_element_fields(session, 0, calibration::EditTargetKind::XAxis);
    const auto spec = fields.spec();
    EXPECT_EQ(spec.address, 64U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::Uint8);
    EXPECT_EQ(spec.from_byte, "x*10");
    EXPECT_EQ(spec.to_byte, "x/10");
    EXPECT_EQ(spec.start_position, 4U);
    EXPECT_EQ(spec.interval, 5U);
    EXPECT_DOUBLE_EQ(spec.fine_increment, 0.1);
}

TEST(MapEditAdapter, BodyStorageAndEndianFallBackToScalingButAxesUseResolvedStorage)
{
    auto def = two_by_two_definition();
    def.maps[0].storage_type.reset();
    def.maps[0].endian.clear();
    def.scalings[0].storage_type = definition::StorageType::Float;
    def.scalings[0].endian = "little";
    def.maps[0].x_axis.scaling_name = "body";
    const auto session = session_from(std::move(def));
    const auto body = collect_map_element_fields(session, 0, calibration::EditTargetKind::MapBody);
    EXPECT_EQ(body.spec().storage_type, definition::StorageType::Float);
    EXPECT_EQ(body.spec().endian, "little");
    const auto axis = collect_map_element_fields(session, 0, calibration::EditTargetKind::XAxis);
    EXPECT_EQ(axis.spec().storage_type, definition::StorageType::Uint8);
    EXPECT_EQ(axis.spec().endian, " ");
}

TEST(MapEditAdapter, MissingScalingAndAxisRetainLegacyPlaceholders)
{
    auto def = two_by_two_definition();
    def.maps[0].scaling_name.clear();
    def.maps[0].x_axis = {};
    const auto session = session_from(std::move(def));
    const auto body = collect_map_element_fields(session, 0, calibration::EditTargetKind::MapBody);
    EXPECT_EQ(body.spec().from_byte, " ");
    const auto axis = collect_map_element_fields(session, 0, calibration::EditTargetKind::XAxis);
    EXPECT_EQ(axis.spec().endian, " ");
    EXPECT_EQ(axis.spec().to_byte, " ");
    EXPECT_EQ(axis.spec().address, 0U);
    EXPECT_EQ(axis.spec().start_position, 1U);
}

TEST(FormatRawElementValue, FormatsUnsignedRawValueAsPlainDecimal)
{
    calibration::MapElementSpec spec;
    spec.storage_type = definition::StorageType::Uint16;

    EXPECT_EQ(format_raw_element_value(spec, 0x1234), "4660");
}

TEST(FormatRawElementValue, FormatsSignedRawValueSignExtended)
{
    calibration::MapElementSpec spec;
    spec.storage_type = definition::StorageType::Int16;

    EXPECT_EQ(format_raw_element_value(spec, -300), "-300");
}

TEST(FormatRawElementValue, FormatsFloatRawValueFromItsBitPattern)
{
    calibration::MapElementSpec spec;
    spec.storage_type = definition::StorageType::Float;

    const auto bits = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(1.5F));

    EXPECT_EQ(format_raw_element_value(spec, bits), "1.5");
}

// Pins a real divergence from master, not one of the established defects:
// get_rom_data_value's storagetype.startsWith("float"/"uint"/"int") chain
// matches nothing for a storage-type string outside the known set, leaving
// `value` an empty QString. storage_type_from_text maps that same
// unrecognized string to std::nullopt; without this branch, is_unsigned_
// storage(nullopt) is false and storage_byte_size(nullopt) is 1, so the
// qint8 case would run and format a spurious number instead of "".
TEST(FormatRawElementValue, ReturnsEmptyStringForAnUnrecognizedStorageType)
{
    calibration::MapElementSpec spec;
    spec.storage_type = std::nullopt;

    EXPECT_EQ(format_raw_element_value(spec, -1), "");
}

TEST(RawElementValueFromText, ParsesNonFloatStorageAsPlainInteger)
{
    calibration::MapElementSpec spec;
    spec.storage_type = definition::StorageType::Uint16;

    EXPECT_EQ(raw_element_value_from_text(spec, "1234"), 1234);
}

// The exact bug this fix wave (step 6b-4) fixed: a fractional float display
// value used to be parsed with QString::toInt(), which fails outright on a
// string like "1.5" (Qt's toInt() requires the whole string to be a valid
// integer) and silently returns 0. raw_element_value_from_text instead
// converts the float VALUE via toFloat(), then bit_casts to get the actual
// IEEE-754 bit pattern write_raw_element expects for float storage.
TEST(RawElementValueFromText, ConvertsFractionalFloatTextToItsBitPatternViaBitCast)
{
    calibration::MapElementSpec spec;
    spec.storage_type = definition::StorageType::Float;

    const auto expected = static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(1.5F));

    EXPECT_EQ(raw_element_value_from_text(spec, "1.5"), expected);
}

// raw_element_value_from_text is the inverse of format_raw_element_value:
// formatting a float's raw bit pattern to text and converting that text
// back must recover the same raw value. 1.5F round-trips exactly at
// QString::number's default (6 significant digit) precision -- a value that
// lost precision at that width would be a separate, pre-existing issue,
// not something this test is checking.
TEST(RawElementValueFromText, RoundTripsWithFormatRawElementValueForFloatStorage)
{
    calibration::MapElementSpec spec;
    spec.storage_type = definition::StorageType::Float;

    const auto raw = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(1.5F));
    const QString text = format_raw_element_value(spec, raw);

    EXPECT_EQ(raw_element_value_from_text(spec, text), raw);
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

TEST(ResolveActiveMapEdit, ResolvesAMapBodySelectionToItsSpecRangeAndCellText)
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

    ASSERT_EQ(edit->cell_text().size(), 5U);
    EXPECT_EQ(edit->cell_text()[0], "2");
    EXPECT_EQ(edit->cell_text()[3], "8");
    EXPECT_EQ(edit->cell_text()[4], "");

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
    const calibration::EditPatch patch{{.index = 0, .byte_address = 18, .bytes = {0, 7}}};
    ASSERT_TRUE(apply_patch(session, 0, calibration::EditTargetKind::MapBody, patch).has_value());
    EXPECT_EQ(edit->cell_text()[0], "2");
    const auto refreshed = resolve_active_map_edit(&window, session, 0);
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_EQ(refreshed->cell_text()[0], "14");
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

TEST(ApplyPatch, DropsInvalidCellIndex)
{
    auto session = session_from();
    const auto before = std::vector<std::uint8_t>(session.rom().begin(), session.rom().end());
    calibration::EditPatch patch{
        {.index = static_cast<std::uint32_t>(-1), .display_text = "999", .byte_address = 64, .bytes = {1, 2}}};
    ASSERT_TRUE(apply_patch(session, 0, calibration::EditTargetKind::XAxis, patch).has_value());
    EXPECT_EQ(std::vector<std::uint8_t>(session.rom().begin(), session.rom().end()), before);
    EXPECT_FALSE(session.dirty());
}

TEST(ApplyPatch, PreservesLegacyTrailingBlankExtent)
{
    auto session = session_from();
    const calibration::EditPatch patch{{.index = 4, .byte_address = 0, .bytes = {9}}};
    ASSERT_TRUE(apply_patch(session, 0, calibration::EditTargetKind::MapBody, patch).has_value());
    EXPECT_EQ(session.rom()[0], 9);
    EXPECT_TRUE(session.dirty());
}

TEST(ApplyPatch, FailedPatchPreservesAlreadyDirtyStateAndRejectsOverflow)
{
    auto session = session_from();
    const std::array<std::uint8_t, 1> initial{9};
    ASSERT_TRUE(session.write_bytes(0, initial).has_value());
    const auto before = std::vector<std::uint8_t>(session.rom().begin(), session.rom().end());
    const calibration::EditPatch patch{{.index = 0, .byte_address = UINT64_MAX, .bytes = {1}}};
    EXPECT_FALSE(apply_patch(session, 0, calibration::EditTargetKind::MapBody, patch).has_value());
    EXPECT_EQ(std::vector<std::uint8_t>(session.rom().begin(), session.rom().end()), before);
    EXPECT_TRUE(session.dirty());
}

TEST(ApplyPatch, ValidatesAllRangesBeforeWriting)
{
    auto session = session_from();
    const auto before = std::vector<std::uint8_t>(session.rom().begin(), session.rom().end());
    calibration::EditPatch patch{{.index = 0, .byte_address = 18, .bytes = {0, 7}},
                                 {.index = 1, .byte_address = 127, .bytes = {0, 8}}};
    EXPECT_FALSE(apply_patch(session, 0, calibration::EditTargetKind::MapBody, patch).has_value());
    EXPECT_EQ(std::vector<std::uint8_t>(session.rom().begin(), session.rom().end()), before);
    EXPECT_FALSE(session.dirty());
}

TEST(ApplyPatch, WritesBodyAndAxesAndDecodesBytesInsteadOfPatchText)
{
    auto session = session_from();
    auto expected = std::vector<std::uint8_t>(session.rom().begin(), session.rom().end());
    expected[19] = 7;
    expected[64] = 3;
    expected[83] = 12;
    const calibration::EditPatch body{{.index = 0, .display_text = "wrong", .byte_address = 18, .bytes = {0, 7}}};
    const calibration::EditPatch x{{.index = 0, .display_text = "wrong", .byte_address = 64, .bytes = {3}}};
    const calibration::EditPatch y{{.index = 1, .display_text = "wrong", .byte_address = 82, .bytes = {0, 12}}};
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
    EXPECT_EQ(decoded->map_data, "14,4,6,8,");
    EXPECT_EQ(decoded->x_axis_data, "30,0,");
    EXPECT_EQ(decoded->y_axis_data, "0,3,");
    EXPECT_TRUE(session.dirty());
}

} // namespace
} // namespace fastecu::ui
