#include "src/ui/desktop/calibration/map_edit_adapter.h"

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
        static char program[] = "map_edit_adapter_test";
        static char *argv[] = {program, nullptr};
        app_ = std::make_unique<QApplication>(argc, argv);
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

definitions::EcuCalDefStructure two_by_two_def()
{
    definitions::EcuCalDefStructure def;
    def.NameList << "Timing";
    def.AddressList << "10000";
    def.StorageTypeList << "uint16";
    def.EndianList << "big";
    def.ToByteList << "x";
    def.FromByteList << "x*2";
    def.MinValueList << " ";
    def.MaxValueList << " ";
    def.CoarseIncList << "1.0";
    def.FineIncList << "0.1";
    def.XSizeList << "2";
    def.YSizeList << "2";
    def.FileSize = "196608";
    return def;
}

TEST(MapEditAdapter, PlucksMapBodyFields)
{
    const auto def = two_by_two_def();

    const auto fields = collect_map_element_fields(def, 0, calibration::EditTargetKind::MapBody);
    const auto spec = fields.spec();

    EXPECT_EQ(spec.address, 0x10000U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::Uint16);
    EXPECT_EQ(spec.endian, "big");
    EXPECT_EQ(spec.from_byte, "x*2");
    EXPECT_DOUBLE_EQ(spec.fine_increment, 0.1);
}

TEST(MapEditAdapter, PlucksXAxisFieldsFromTheXScaleLists)
{
    definitions::EcuCalDefStructure def = two_by_two_def();
    def.XScaleAddressList << "20000";
    def.XScaleStorageTypeList << "uint8";
    def.XScaleEndianList << "little";
    def.XScaleToByteList << "x/10";
    def.XScaleFromByteList << "x*10";
    def.XScaleMinValueList << " ";
    def.XScaleMaxValueList << " ";
    def.XScaleCoarseIncList << "2.0";
    def.XScaleFineIncList << "0.5";

    const auto fields = collect_map_element_fields(def, 0, calibration::EditTargetKind::XAxis);
    const auto spec = fields.spec();

    EXPECT_EQ(spec.address, 0x20000U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::Uint8);
    EXPECT_EQ(spec.endian, "little");
    EXPECT_EQ(spec.to_byte, "x/10");
    EXPECT_EQ(spec.from_byte, "x*10");
    EXPECT_DOUBLE_EQ(spec.coarse_increment, 2.0);
    EXPECT_DOUBLE_EQ(spec.fine_increment, 0.5);
}

TEST(MapEditAdapter, PlucksYAxisFieldsFromTheYScaleLists)
{
    definitions::EcuCalDefStructure def = two_by_two_def();
    def.YScaleAddressList << "30000";
    def.YScaleStorageTypeList << "int16";
    def.YScaleEndianList << "big";
    def.YScaleToByteList << "x*4";
    def.YScaleFromByteList << "x/4";
    def.YScaleMinValueList << " ";
    def.YScaleMaxValueList << " ";
    def.YScaleCoarseIncList << "3.0";
    def.YScaleFineIncList << "0.25";

    const auto fields = collect_map_element_fields(def, 0, calibration::EditTargetKind::YAxis);
    const auto spec = fields.spec();

    EXPECT_EQ(spec.address, 0x30000U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::Int16);
    EXPECT_EQ(spec.endian, "big");
    EXPECT_EQ(spec.to_byte, "x*4");
    EXPECT_EQ(spec.from_byte, "x/4");
    EXPECT_DOUBLE_EQ(spec.coarse_increment, 3.0);
    EXPECT_DOUBLE_EQ(spec.fine_increment, 0.25);
}

TEST(MapEditAdapter, ReadsFlashMethodAndRomFileSizeFromTheSharedRomInfo)
{
    definitions::EcuCalDefStructure def = two_by_two_def();
    def.RomInfo.clear();
    for (int i = 0; i < 16; ++i)
    {
        def.RomInfo << "";
    }
    def.RomInfo[10] = "wrx02";
    def.FileSize = "12345";

    const auto fields = collect_map_element_fields(def, 0, calibration::EditTargetKind::MapBody);
    const auto spec = fields.spec();

    EXPECT_EQ(spec.flash_method, "wrx02");
    EXPECT_EQ(spec.rom_file_size, 12345U);
}

// Only the fallback value is asserted here; collect_map_element_fields also
// logs a qWarning on this path (not observed by this test -- would need a
// custom Qt message handler installed for the duration of the test).
TEST(MapEditAdapter, FallsBackToZeroOnABadHexAddress)
{
    definitions::EcuCalDefStructure def = two_by_two_def();
    def.AddressList[0] = "not-hex";

    const auto fields = collect_map_element_fields(def, 0, calibration::EditTargetKind::MapBody);
    const auto spec = fields.spec();

    EXPECT_EQ(spec.address, 0U);
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

TEST(ParseMapWindowId, ParsesRomAndMapNumberFromAWellFormedObjectName)
{
    QMdiSubWindow window;
    window.setObjectName("2,7,Timing,uint16");

    const auto id = parse_map_window_id(&window);

    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->rom_number, 2);
    EXPECT_EQ(id->map_number, 7);
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

definitions::EcuCalDefStructure two_by_two_map_body_def()
{
    auto def = two_by_two_def();
    def.MapData << "1,2,3,4";
    def.XScaleTypeList << "Linear";
    return def;
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
    EXPECT_FALSE(resolve_active_map_edit(nullptr, two_by_two_map_body_def(), 0).has_value());
}

TEST(ResolveActiveMapEdit, ReturnsNulloptWhenNoMatchingTableWidgetIsFound)
{
    QMdiSubWindow window;
    window.setObjectName("0,0,Timing,uint16");
    // Deliberately no QTableWidget child added.

    EXPECT_FALSE(resolve_active_map_edit(&window, two_by_two_map_body_def(), 0).has_value());
}

TEST(ResolveActiveMapEdit, ReturnsNulloptWhenTheSelectionIsEmpty)
{
    QMdiSubWindow window;
    build_map_window(window, 3, 3);

    EXPECT_FALSE(resolve_active_map_edit(&window, two_by_two_map_body_def(), 0).has_value());
}

TEST(ResolveActiveMapEdit, ResolvesAMapBodySelectionToItsSpecRangeAndCellText)
{
    QMdiSubWindow window;
    auto *table = build_map_window(window, 3, 3);
    // Widget row/col 0 are axis headers; (1, 1) is the top-left data cell.
    table->setRangeSelected(QTableWidgetSelectionRange(1, 1, 1, 1), true);

    const auto def = two_by_two_map_body_def();
    const auto edit = resolve_active_map_edit(&window, def, 0);

    ASSERT_TRUE(edit.has_value());
    EXPECT_EQ(edit->kind(), calibration::EditTargetKind::MapBody);
    EXPECT_EQ(edit->map_number(), 0);
    EXPECT_EQ(edit->x_size(), 2U);
    EXPECT_EQ(edit->range().first_row, 0);
    EXPECT_EQ(edit->range().first_col, 0);
    EXPECT_EQ(edit->range().last_row, 0);
    EXPECT_EQ(edit->range().last_col, 0);

    ASSERT_EQ(edit->cell_text().size(), 4U);
    EXPECT_EQ(edit->cell_text()[0], "1");
    EXPECT_EQ(edit->cell_text()[3], "4");

    const auto spec = edit->spec();
    EXPECT_EQ(spec.address, 0x10000U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::Uint16);
}

TEST(ResolveActiveMapEdit, ReturnsNulloptForAStaticAxisSelection)
{
    QMdiSubWindow window;
    auto *table = build_map_window(window, 3, 3);
    // Column 0 with a multi-row map targets the Y axis, which is rejected
    // when the definition marks it static.
    table->setRangeSelected(QTableWidgetSelectionRange(1, 0, 1, 0), true);

    auto def = two_by_two_map_body_def();
    def.XScaleTypeList[0] = "Static Y Axis";

    EXPECT_FALSE(resolve_active_map_edit(&window, def, 0).has_value());
}

} // namespace
} // namespace fastecu::ui
