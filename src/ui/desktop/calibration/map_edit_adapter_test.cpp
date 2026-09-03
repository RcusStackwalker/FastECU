#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <bit>

#include <gtest/gtest.h>

namespace fastecu::ui
{
namespace
{

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

} // namespace
} // namespace fastecu::ui
