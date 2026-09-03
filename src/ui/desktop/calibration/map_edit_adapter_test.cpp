#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <QtTest>
#include <gtest/gtest.h>

namespace fastecu::ui
{
namespace
{

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

TEST(MapEditAdapter, PlucksMapBodyFieldsAndKeepsThemAlive)
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

TEST(MapEditAdapter, ReportsAndFallsBackToZeroOnABadHexAddress)
{
    definitions::EcuCalDefStructure def = two_by_two_def();
    def.AddressList[0] = "not-hex";

    const auto fields = collect_map_element_fields(def, 0, calibration::EditTargetKind::MapBody);
    const auto spec = fields.spec();

    EXPECT_EQ(spec.address, 0U);
}

} // namespace
} // namespace fastecu::ui
