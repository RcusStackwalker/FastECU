#include "src/backend/definition/legacy_definition_adapter.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace fastecu::definition
{
namespace
{

std::vector<std::uint8_t> bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}

class FakeFileSystem : public IFileSystem
{
  public:
    bool exists(std::string_view path) override
    {
        return directories.contains(std::string(path));
    }

    Status create_directory(std::string_view) override
    {
        return {};
    }

    Status copy_file(std::string_view, std::string_view, bool) override
    {
        return {};
    }

    Status remove_file(std::string_view) override
    {
        return {};
    }

    Result<std::vector<DirEntry>> list_directory(std::string_view path) override
    {
        const std::string key(path);
        if (auto error = list_errors.find(key); error != list_errors.end())
        {
            return std::unexpected(error->second);
        }
        if (auto directory = directories.find(key); directory != directories.end())
        {
            return directory->second;
        }
        return fail(ErrorKind::InvalidConfig, "unknown directory: " + key);
    }

    std::map<std::string, std::vector<DirEntry>> directories;
    std::map<std::string, Error> list_errors;
};

class FakeFileRepository : public IFileRepository
{
  public:
    Result<std::vector<std::uint8_t>> read(std::string_view handle) override
    {
        const std::string key(handle);
        ++read_counts[key];
        if (auto error = read_errors.find(key); error != read_errors.end())
        {
            return std::unexpected(error->second);
        }
        if (auto file = files.find(key); file != files.end())
        {
            return file->second;
        }
        return fail(ErrorKind::InvalidConfig, "unknown file: " + key);
    }

    Status write(std::string_view, std::span<const std::uint8_t>) override
    {
        return {};
    }

    std::map<std::string, std::vector<std::uint8_t>> files;
    std::map<std::string, Error> read_errors;
    std::map<std::string, int> read_counts;
};

class FakeAtomicFileWriter : public IAtomicFileWriter
{
  public:
    Status replace(std::string_view handle, std::span<const std::uint8_t> data) override
    {
        calls.emplace_back(std::string(handle), std::vector<std::uint8_t>(data.begin(), data.end()));
        if (error)
        {
            return std::unexpected(*error);
        }
        return {};
    }

    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> calls;
    std::optional<Error> error;
};

class LegacyDefinitionAdapterTest : public ::testing::Test
{
  protected:
    FakeFileSystem file_system;
    FakeFileRepository repository;
    FakeAtomicFileWriter writer;
    DefinitionService service{file_system, repository, writer};
    LegacyDefinitionAdapter adapter{service};
};

void expect_catalog_rows_aligned(
    const QStringList& ids,
    const QStringList& addresses,
    const QStringList& ecu_ids,
    const QStringList& filenames)
{
    EXPECT_EQ(addresses.size(), ids.size());
    EXPECT_EQ(ecu_ids.size(), ids.size());
    EXPECT_EQ(filenames.size(), ids.size());
}

void expect_map_rows_aligned(const definitions::EcuCalDefStructure& value)
{
    const qsizetype rows = value.NameList.size();
    const std::vector<const QStringList *> lists{
        &value.IdList,
        &value.TypeList,
        &value.AddressList,
        &value.CategoryList,
        &value.CategoryExpandedList,
        &value.SubCategoryList,
        &value.LevelList,
        &value.UserLevelList,
        &value.SwapXYList,
        &value.FlipXList,
        &value.FlipYList,
        &value.XSizeList,
        &value.YSizeList,
        &value.StartPosList,
        &value.IntervalList,
        &value.MinValueList,
        &value.MaxValueList,
        &value.UnitsList,
        &value.FormatList,
        &value.FineIncList,
        &value.CoarseIncList,
        &value.VisibleList,
        &value.SelectionsNameList,
        &value.SelectionsValueList,
        &value.DescriptionList,
        &value.StateList,
        &value.MapScalingNameList,
        &value.MapData,
        &value.MapCellColorMin,
        &value.MapCellColorMax,
        &value.XScaleTypeList,
        &value.XScaleNameList,
        &value.XScaleAddressList,
        &value.XScaleStartPosList,
        &value.XScaleIntervalList,
        &value.XScaleMinValueList,
        &value.XScaleMaxValueList,
        &value.XScaleUnitsList,
        &value.XScaleFormatList,
        &value.XScaleFineIncList,
        &value.XScaleCoarseIncList,
        &value.XScaleStorageTypeList,
        &value.XScaleEndianList,
        &value.XScaleLogParamList,
        &value.XScaleFromByteList,
        &value.XScaleToByteList,
        &value.XScaleStaticDataList,
        &value.XScaleScalingNameList,
        &value.XScaleData,
        &value.YScaleTypeList,
        &value.YScaleNameList,
        &value.YScaleAddressList,
        &value.YScaleStartPosList,
        &value.YScaleIntervalList,
        &value.YScaleMinValueList,
        &value.YScaleMaxValueList,
        &value.YScaleUnitsList,
        &value.YScaleFormatList,
        &value.YScaleFineIncList,
        &value.YScaleCoarseIncList,
        &value.YScaleStorageTypeList,
        &value.YScaleEndianList,
        &value.YScaleLogParamList,
        &value.YScaleFromByteList,
        &value.YScaleToByteList,
        &value.YScaleScalingNameList,
        &value.YScaleData,
        &value.StorageTypeList,
        &value.EndianList,
        &value.LogParamList,
        &value.FromByteList,
        &value.ToByteList,
        &value.MapDefined,
    };
    for (const QStringList *list : lists)
    {
        EXPECT_EQ(list->size(), rows);
    }
}

void expect_scaling_rows_aligned(const definitions::EcuCalDefStructure& value)
{
    const qsizetype rows = value.ScalingNameList.size();
    const std::vector<const QStringList *> lists{
        &value.ScalingUnitsList,
        &value.ScalingFromByteList,
        &value.ScalingToByteList,
        &value.ScalingFormatList,
        &value.ScalingMinValueList,
        &value.ScalingMaxValueList,
        &value.ScalingCoarseIncList,
        &value.ScalingFineIncList,
        &value.ScalingStorageTypeList,
        &value.ScalingEndianList,
        &value.ScalingSelectionsNameList,
        &value.ScalingSelectionsValueList,
    };
    for (const QStringList *list : lists)
    {
        EXPECT_EQ(list->size(), rows);
    }
}

DefinitionIndexEntry entry(
    DefinitionFormat format,
    std::string id,
    std::string source)
{
    return DefinitionIndexEntry{
        .format = format,
        .definition_id = std::move(id),
        .internal_id_encoding = IdEncoding::Ascii,
        .source = std::move(source),
    };
}

TEST_F(LegacyDefinitionAdapterTest, ReplacesRomRaiderCatalogWithAlignedTypedRows)
{
    repository.files["second.xml"] = bytes(
        "<roms><rom><romid><xmlid>SECOND</xmlid><internalidaddress>2A0</internalidaddress>"
        "<ecuid>ECU-2</ecuid></romid></rom></roms>");
    repository.files["first.xml"] = bytes(
        "<roms><rom><romid><xmlid>FIRST</xmlid><internalidaddress>1A0</internalidaddress>"
        "<ecuid>ECU-1</ecuid></romid></rom></roms>");
    const std::vector<std::string> handles{"second.xml", "first.xml"};
    definitions::ConfigValuesStructure value;
    value.romraider_def_cal_id = {"sentinel-id"};
    value.romraider_def_cal_id_addr = {"sentinel-address"};
    value.romraider_def_ecu_id = {"sentinel-ecu"};
    value.romraider_def_filename = {"sentinel-file"};

    auto result = adapter.replace_romraider_catalog(value, handles);

    ASSERT_TRUE(result);
    EXPECT_EQ(value.romraider_def_cal_id, QStringList({"SECOND", "FIRST"}));
    EXPECT_EQ(value.romraider_def_cal_id_addr, QStringList({"0x2a0", "0x1a0"}));
    EXPECT_EQ(value.romraider_def_ecu_id, QStringList({"ECU-2", "ECU-1"}));
    EXPECT_EQ(value.romraider_def_filename, QStringList({"second.xml", "first.xml"}));
    expect_catalog_rows_aligned(
        value.romraider_def_cal_id,
        value.romraider_def_cal_id_addr,
        value.romraider_def_ecu_id,
        value.romraider_def_filename);
}

TEST_F(LegacyDefinitionAdapterTest, ReplacesEcuFlashCatalogWithAlignedTypedRows)
{
    file_system.directories["defs"] = {
        DirEntry{.name = "b.xml", .is_directory = false},
        DirEntry{.name = "a.xml", .is_directory = false},
    };
    repository.files["defs/a.xml"] = bytes(
        "<rom><romid><xmlid>A</xmlid><internalidaddress>10</internalidaddress>"
        "<ecuid>ECU-A</ecuid></romid></rom>");
    repository.files["defs/b.xml"] = bytes(
        "<rom><romid><xmlid>B</xmlid><ecuid>ECU-B</ecuid></romid></rom>");
    repository.files["outside.xml"] = bytes(
        "<rom><romid><xmlid>OUTSIDE</xmlid><internalidaddress>20</internalidaddress>"
        "<ecuid>ECU-OUTSIDE</ecuid></romid></rom>");
    definitions::ConfigValuesStructure value;
    value.ecuflash_def_cal_id = {"sentinel-id"};
    value.ecuflash_def_cal_id_addr = {"sentinel-address"};
    value.ecuflash_def_ecu_id = {"sentinel-ecu"};
    value.ecuflash_def_filename = {"outside.xml", "defs/a.xml"};
    const std::vector<std::string> explicit_handles{
        "outside.xml",
        "defs/a.xml",
    };

    auto result =
        adapter.replace_ecuflash_catalog(value, "defs", explicit_handles);

    ASSERT_TRUE(result);
    EXPECT_EQ(
        value.ecuflash_def_cal_id,
        QStringList({"A", "B", "OUTSIDE"}));
    EXPECT_EQ(
        value.ecuflash_def_cal_id_addr,
        QStringList({"0x10", "", "0x20"}));
    EXPECT_EQ(
        value.ecuflash_def_ecu_id,
        QStringList({"ECU-A", "ECU-B", "ECU-OUTSIDE"}));
    EXPECT_EQ(
        value.ecuflash_def_filename,
        QStringList({"defs/a.xml", "defs/b.xml", "outside.xml"}));
    expect_catalog_rows_aligned(
        value.ecuflash_def_cal_id,
        value.ecuflash_def_cal_id_addr,
        value.ecuflash_def_ecu_id,
        value.ecuflash_def_filename);
    EXPECT_EQ(repository.read_counts["defs/a.xml"], 1);
    EXPECT_EQ(repository.read_counts["defs/b.xml"], 1);
    EXPECT_EQ(repository.read_counts["outside.xml"], 1);
}

TEST_F(LegacyDefinitionAdapterTest, EcuFlashCatalogFailurePreservesCompleteOriginalValue)
{
    file_system.directories["defs"] = {};
    repository.read_errors["outside.xml"] =
        Error{ErrorKind::Disconnected, "submitted definition unavailable"};
    definitions::ConfigValuesStructure value;
    value.software_name = "unchanged software";
    value.primary_definition_base = "unchanged base";
    value.romraider_def_cal_id = {"romraider-id"};
    value.romraider_def_filename = {"romraider.xml"};
    value.ecuflash_def_cal_id = {"outside-id"};
    value.ecuflash_def_cal_id_addr = {"outside-address"};
    value.ecuflash_def_ecu_id = {"outside-ecu"};
    value.ecuflash_def_filename = {"outside.xml"};
    const auto original = value;
    const std::vector<std::string> explicit_handles{"outside.xml"};

    auto result =
        adapter.replace_ecuflash_catalog(value, "defs", explicit_handles);

    ASSERT_FALSE(result);
    EXPECT_EQ(
        result.error(),
        (Error{
            ErrorKind::Disconnected,
            "submitted definition unavailable",
        }));
    EXPECT_EQ(value, original);
}

TEST_F(LegacyDefinitionAdapterTest, CatalogFailurePreservesCompleteOriginalValue)
{
    repository.read_errors["bad.xml"] = Error{ErrorKind::Disconnected, "read failed"};
    definitions::ConfigValuesStructure value;
    value.software_name = "unchanged software";
    value.primary_definition_base = "unchanged base";
    value.romraider_def_cal_id = {"sentinel-id"};
    value.romraider_def_cal_id_addr = {"sentinel-address"};
    value.romraider_def_ecu_id = {"sentinel-ecu"};
    value.romraider_def_filename = {"sentinel-file"};
    value.ecuflash_def_cal_id = {"other-id"};
    value.ecuflash_def_cal_id_addr = {"other-address"};
    value.ecuflash_def_ecu_id = {"other-ecu"};
    value.ecuflash_def_filename = {"other-file"};
    const auto original = value;

    auto result = adapter.replace_romraider_catalog(value, std::vector<std::string>{"bad.xml"});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), (Error{ErrorKind::Disconnected, "read failed"}));
    EXPECT_EQ(value, original);
}

TEST_F(LegacyDefinitionAdapterTest, MapsFullTypedDefinitionIntoEveryLegacySlice)
{
    repository.files["full.xml"] = bytes(R"xml(
      <rom><romid><xmlid>FULL</xmlid><internalidaddress>100</internalidaddress>
        <internalidstring>FULL-ID</internalidstring><ecuid>ECU-A</ecuid>
        <make>Subaru</make><market>USDM</market><model>Legacy</model>
        <submodel>GT</submodel><transmission>6MT</transmission><year>2008</year>
        <flashmethod>denso</flashmethod><memmodel>SH7058</memmodel>
        <checksummodule>subaru</checksummodule><filesize>1048576</filesize>
        <notes>All metadata</notes></romid>
        <include>MID</include>
        <scaling name="mode-scale" units="state" toexpr="x" frexpr="x"
                 format="%d" min="0" max="1" inc="1" storagetype="bloblist" endian="big">
          <data name="disabled" value="00"/><data name="enabled" data="01"/>
        </scaling>
        <scaling name="fuel-scale" units="%" toexpr="x*0.5" frexpr="x*2"
                 format="%.1f" min="0" max="100" inc="1" storagetype="uint16" endian="big"/>
        <table id="mode-id" name="Mode" address="210" type="1D"
               category="Switches" subcategory="Drive" description="Mode selection"
               level="1" userlevel="2" sizex="1" sizey="1"
               swapxy="false" flipx="true" flipy="false" scaling="mode-scale"
               startpos="11" interval="2"/>
        <table id="fuel-id" name="Fuel" address="220" type="2D"
               category="Fuel" subcategory="Primary" description="Fuel map"
               level="3" userlevel="4" sizex="4" sizey="1"
               swapxy="true" flipx="false" flipy="true" scaling="fuel-scale"
               startpos="12" interval="3">
          <table type="Static X Axis" name="Engine Speed" address="300" elements="4"
                 startpos="21" interval="5">
            <scaling name="rpm-scale" units="rpm" toexpr="x" frexpr="x"
                     format="%d" min="0" max="8000" inc="100"
                     storagetype="uint16" endian="big"/>
            <data>1000</data><data>2000</data><data>3000</data><data>4000</data>
          </table>
        </table>
        <table id="timing-id" name="Timing" address="230" type="3D" sizex="2" sizey="2"
               startpos="13" interval="4">
          <table type="X Axis" name="Load" address="310" elements="2"
                 startpos="22" interval="6">
            <scaling name="load-scale" units="g/rev" toexpr="x/10" frexpr="x*10"
                     format="%.2f" min="0" max="4" inc="0.1" storagetype="uint8" endian="little"/>
          </table>
          <table type="Y Axis" name="Temperature" address="320" elements="2"
                 startpos="23" interval="7">
            <scaling name="temp-scale" units="C" toexpr="x-40" frexpr="x+40"
                     format="%d" min="-40" max="200" inc="1" storagetype="uint8" endian="little"/>
          </table>
        </table>
      </rom>)xml");
    repository.files["mid.xml"] =
        bytes("<rom><romid><xmlid>MID</xmlid></romid><include>BASE</include></rom>");
    repository.files["base.xml"] =
        bytes("<rom><romid><xmlid>BASE</xmlid></romid></rom>");
    auto catalog = DefinitionCatalog::create({
        entry(DefinitionFormat::EcuFlash, "FULL", "full.xml"),
        entry(DefinitionFormat::EcuFlash, "MID", "mid.xml"),
        entry(DefinitionFormat::EcuFlash, "BASE", "base.xml"),
    });
    ASSERT_TRUE(catalog);
    definitions::EcuCalDefStructure value;
    value.FileName = "rom.bin";
    value.FullFileName = "/roms/rom.bin";
    value.RomId = "identified-rom";
    value.Kernel = "kernel.bin";
    value.NameList = {"sentinel-map"};
    value.ScalingNameList = {"sentinel-scale"};
    value.RomInfo = {"sentinel"};
    value.OemEcuFile = true;
    value.SyncedWithEcu = false;
    value.use_romraider_definition = true;
    value.use_ecuflash_definition = false;

    auto result = adapter.replace_definition(
        value, *catalog, DefinitionFormat::EcuFlash, "FULL");

    ASSERT_TRUE(result);
    EXPECT_EQ(value.FileName, "rom.bin");
    EXPECT_EQ(value.FullFileName, "/roms/rom.bin");
    EXPECT_EQ(value.RomId, "identified-rom");
    EXPECT_EQ(value.Kernel, "kernel.bin");
    EXPECT_TRUE(value.OemEcuFile);
    EXPECT_FALSE(value.SyncedWithEcu);
    EXPECT_FALSE(value.use_romraider_definition);
    EXPECT_TRUE(value.use_ecuflash_definition);
    EXPECT_EQ(value.DefinitionFileName, "full.xml");
    EXPECT_EQ(value.RomBase, "BASE");

    ASSERT_EQ(value.RomInfo.size(), value.RomInfoStrings.size());
    EXPECT_EQ(value.RomInfo.at(0), "FULL");
    EXPECT_EQ(value.RomInfo.at(1), "0x100");
    EXPECT_EQ(value.RomInfo.at(2), "FULL-ID");
    EXPECT_EQ(value.RomInfo.at(3), "ECU-A");
    EXPECT_EQ(value.RomInfo.at(4), "Subaru");
    EXPECT_EQ(value.RomInfo.at(5), "USDM");
    EXPECT_EQ(value.RomInfo.at(6), "Legacy");
    EXPECT_EQ(value.RomInfo.at(7), "GT");
    EXPECT_EQ(value.RomInfo.at(8), "6MT");
    EXPECT_EQ(value.RomInfo.at(9), "2008");
    EXPECT_EQ(value.RomInfo.at(10), "denso");
    EXPECT_EQ(value.RomInfo.at(11), "SH7058");
    EXPECT_EQ(value.RomInfo.at(12), "subaru");
    EXPECT_EQ(value.RomInfo.at(13), "BASE");
    EXPECT_EQ(value.RomInfo.at(14), "1048576");
    EXPECT_EQ(value.RomInfo.at(15), "full.xml");

    EXPECT_EQ(value.IdList, QStringList({"mode-id", "fuel-id", "timing-id"}));
    EXPECT_EQ(value.NameList, QStringList({"Mode", "Fuel", "Timing"}));
    EXPECT_EQ(value.TypeList, QStringList({"Selectable", "2D", "3D"}));
    EXPECT_EQ(value.AddressList, QStringList({"0x210", "0x220", "0x230"}));
    EXPECT_EQ(value.CategoryList, QStringList({"Switches", "Fuel", " "}));
    EXPECT_EQ(value.SubCategoryList, QStringList({"Drive", "Primary", " "}));
    EXPECT_EQ(value.DescriptionList, QStringList({"Mode selection", "Fuel map", " "}));
    EXPECT_EQ(value.LevelList, QStringList({"1", "3", " "}));
    EXPECT_EQ(value.UserLevelList, QStringList({"2", "4", " "}));
    EXPECT_EQ(value.SwapXYList, QStringList({"false", "true", "false"}));
    EXPECT_EQ(value.FlipXList, QStringList({"true", "false", "false"}));
    EXPECT_EQ(value.FlipYList, QStringList({"false", "true", "false"}));
    EXPECT_EQ(value.XSizeList, QStringList({"1", "4", "2"}));
    EXPECT_EQ(value.YSizeList, QStringList({"1", "1", "2"}));
    EXPECT_EQ(value.StartPosList, QStringList({"11", "12", "13"}));
    EXPECT_EQ(value.IntervalList, QStringList({"2", "3", "4"}));
    EXPECT_EQ(value.LogParamList, QStringList({" ", " ", " "}));
    EXPECT_EQ(value.MapDefined, QStringList({" ", " ", " "}));
    EXPECT_EQ(value.MapScalingNameList, QStringList({"mode-scale", "fuel-scale", " "}));
    EXPECT_EQ(value.StorageTypeList, QStringList({"bloblist", "uint16", " "}));
    EXPECT_EQ(value.UnitsList, QStringList({"state", "%", " "}));
    EXPECT_EQ(value.FormatList, QStringList({"0", "0.0", " "}));
    EXPECT_EQ(value.FineIncList, QStringList({"0.1", "0.1", " "}));
    EXPECT_EQ(value.CoarseIncList, QStringList({"1", "1", " "}));
    EXPECT_EQ(value.MinValueList, QStringList({"0", "0", " "}));
    EXPECT_EQ(value.MaxValueList, QStringList({"1", "100", " "}));
    EXPECT_EQ(value.EndianList, QStringList({"big", "big", " "}));
    EXPECT_EQ(value.FromByteList, QStringList({"x", "x*0.5", " "}));
    EXPECT_EQ(value.ToByteList, QStringList({"x", "x*2", " "}));
    EXPECT_EQ(value.SelectionsNameList, QStringList({"disabled,enabled,", " ", " "}));
    EXPECT_EQ(value.SelectionsValueList, QStringList({"00,01,", " ", " "}));

    EXPECT_EQ(value.XScaleTypeList, QStringList({" ", "Static X Axis", "X Axis"}));
    EXPECT_EQ(value.XScaleNameList, QStringList({" ", "Engine Speed", "Load"}));
    EXPECT_EQ(value.XScaleAddressList, QStringList({" ", "0x300", "0x310"}));
    EXPECT_EQ(value.XScaleScalingNameList, QStringList({" ", "rpm-scale", "load-scale"}));
    EXPECT_EQ(value.XScaleUnitsList, QStringList({" ", "rpm", "g/rev"}));
    EXPECT_EQ(value.XScaleFormatList, QStringList({" ", "0", "0.00"}));
    EXPECT_EQ(value.XScaleStorageTypeList, QStringList({" ", "uint16", "uint8"}));
    EXPECT_EQ(value.XScaleEndianList, QStringList({" ", "big", "little"}));
    EXPECT_EQ(value.XScaleFromByteList, QStringList({" ", "x", "x/10"}));
    EXPECT_EQ(value.XScaleToByteList, QStringList({" ", "x", "x*10"}));
    EXPECT_EQ(value.XScaleStartPosList, QStringList({" ", "21", "22"}));
    EXPECT_EQ(value.XScaleIntervalList, QStringList({" ", "5", "6"}));
    EXPECT_EQ(value.XScaleLogParamList, QStringList({" ", " ", " "}));
    EXPECT_EQ(value.XScaleStaticDataList, QStringList({" ", "1000,2000,3000,4000,", " "}));

    EXPECT_EQ(value.YScaleTypeList, QStringList({" ", " ", "Y Axis"}));
    EXPECT_EQ(value.YScaleNameList, QStringList({" ", " ", "Temperature"}));
    EXPECT_EQ(value.YScaleAddressList, QStringList({" ", " ", "0x320"}));
    EXPECT_EQ(value.YScaleScalingNameList, QStringList({" ", " ", "temp-scale"}));
    EXPECT_EQ(value.YScaleUnitsList, QStringList({" ", " ", "C"}));
    EXPECT_EQ(value.YScaleFormatList, QStringList({" ", " ", "0"}));
    EXPECT_EQ(value.YScaleStorageTypeList, QStringList({" ", " ", "uint8"}));
    EXPECT_EQ(value.YScaleEndianList, QStringList({" ", " ", "little"}));
    EXPECT_EQ(value.YScaleFromByteList, QStringList({" ", " ", "x-40"}));
    EXPECT_EQ(value.YScaleToByteList, QStringList({" ", " ", "x+40"}));
    EXPECT_EQ(value.YScaleStartPosList, QStringList({" ", " ", "23"}));
    EXPECT_EQ(value.YScaleIntervalList, QStringList({" ", " ", "7"}));
    EXPECT_EQ(value.YScaleLogParamList, QStringList({" ", " ", " "}));

    ASSERT_EQ(value.ScalingNameList.size(), 5);
    EXPECT_EQ(value.ScalingNameList.at(0), "mode-scale");
    EXPECT_EQ(value.ScalingSelectionsNameList.at(0), "disabled,enabled,");
    EXPECT_EQ(value.ScalingSelectionsValueList.at(0), "00,01,");
    EXPECT_EQ(value.ScalingNameList.at(1), "fuel-scale");
    EXPECT_EQ(value.ScalingUnitsList.at(1), "%");
    EXPECT_EQ(value.ScalingFromByteList.at(1), "x*0.5");
    EXPECT_EQ(value.ScalingToByteList.at(1), "x*2");
    EXPECT_EQ(value.ScalingFormatList.at(1), "0.0");
    EXPECT_EQ(value.ScalingMinValueList.at(1), "0");
    EXPECT_EQ(value.ScalingMaxValueList.at(1), "100");
    EXPECT_EQ(value.ScalingCoarseIncList.at(1), "1");
    EXPECT_EQ(value.ScalingFineIncList.at(1), "0.1");
    EXPECT_EQ(value.ScalingStorageTypeList.at(1), "uint16");
    EXPECT_EQ(value.ScalingEndianList.at(1), "big");
    expect_map_rows_aligned(value);
    expect_scaling_rows_aligned(value);
}

TEST_F(LegacyDefinitionAdapterTest, MapsRomRaiderRuntimeLogParameters)
{
    repository.files["rr.xml"] = bytes(R"xml(
      <roms><rom><romid><xmlid>RR</xmlid></romid>
      <table id="fuel" name="Fuel" type="2D" sizex="2" sizey="1"
             startpos="7" interval="2" logparam="P_MAP">
        <table type="Static X Axis" name="Load" elements="2"
               startpos="9" interval="4" logparam="P_AXIS">
          <data>0.5</data><data>1.0</data>
        </table>
      </table></rom></roms>)xml");
    auto catalog = DefinitionCatalog::create({
        entry(DefinitionFormat::RomRaider, "RR", "rr.xml"),
    });
    ASSERT_TRUE(catalog);
    definitions::EcuCalDefStructure value;
    value.OemEcuFile = false;
    value.SyncedWithEcu = false;
    value.use_romraider_definition = false;
    value.use_ecuflash_definition = false;

    auto result = adapter.replace_definition(
        value, *catalog, DefinitionFormat::RomRaider, "RR");

    ASSERT_TRUE(result);
    EXPECT_EQ(value.StartPosList, QStringList({"7"}));
    EXPECT_EQ(value.IntervalList, QStringList({"2"}));
    EXPECT_EQ(value.LogParamList, QStringList({"P_MAP"}));
    EXPECT_EQ(value.XScaleStartPosList, QStringList({"9"}));
    EXPECT_EQ(value.XScaleIntervalList, QStringList({"4"}));
    EXPECT_EQ(value.XScaleLogParamList, QStringList({"P_AXIS"}));
    EXPECT_EQ(value.XScaleStaticDataList, QStringList({"0.5,1.0,"}));
    EXPECT_EQ(value.MapDefined, QStringList({" "}));
}

TEST_F(LegacyDefinitionAdapterTest, ShortRomInfoShapeFailsAtomically)
{
    repository.files["shape.xml"] =
        bytes("<rom><romid><xmlid>SHAPE</xmlid></romid></rom>");
    auto catalog = DefinitionCatalog::create({
        entry(DefinitionFormat::EcuFlash, "SHAPE", "shape.xml"),
    });
    ASSERT_TRUE(catalog);
    definitions::EcuCalDefStructure value;
    value.FileName = "rom.bin";
    value.NameList = {"sentinel-map"};
    value.ScalingNameList = {"sentinel-scale"};
    value.RomInfo = {"sentinel-info"};
    value.RomInfoStrings = {"too short"};
    value.OemEcuFile = true;
    value.SyncedWithEcu = true;
    value.use_romraider_definition = true;
    value.use_ecuflash_definition = false;
    const auto original = value;

    auto result = adapter.replace_definition(
        value, *catalog, DefinitionFormat::EcuFlash, "SHAPE");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("RomInfo"), std::string::npos);
    EXPECT_EQ(value, original);
}

TEST_F(LegacyDefinitionAdapterTest, InvalidStaticAxisFailsAtomically)
{
    repository.files["invalid-static.xml"] = bytes(R"xml(
      <rom>
        <romid><xmlid>INVALID_STATIC</xmlid></romid>
        <table id="load" name="Load" type="2D" sizex="2" sizey="1">
          <table type="Static X Axis" name="Breakpoints" elements="2"/>
        </table>
      </rom>)xml");
    auto catalog = DefinitionCatalog::create({
        entry(
            DefinitionFormat::EcuFlash,
            "INVALID_STATIC",
            "invalid-static.xml"),
    });
    ASSERT_TRUE(catalog);
    definitions::EcuCalDefStructure value;
    value.FileName = "rom.bin";
    value.NameList = {"sentinel-map"};
    value.ScalingNameList = {"sentinel-scale"};
    value.RomInfo = {"sentinel-info"};
    value.OemEcuFile = true;
    value.SyncedWithEcu = true;
    value.use_romraider_definition = true;
    value.use_ecuflash_definition = false;
    const auto original = value;

    auto result = adapter.replace_definition(
        value,
        *catalog,
        DefinitionFormat::EcuFlash,
        "INVALID_STATIC");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("static data"), std::string::npos);
    EXPECT_EQ(value, original);
}

TEST_F(LegacyDefinitionAdapterTest, DefinitionLoadFailureDoesNotMutateCaller)
{
    auto catalog = DefinitionCatalog::create({
        entry(DefinitionFormat::EcuFlash, "MISSING", "missing.xml"),
    });
    ASSERT_TRUE(catalog);
    repository.read_errors["missing.xml"] =
        Error{ErrorKind::Disconnected, "definition read failed"};
    definitions::EcuCalDefStructure value;
    value.FileName = "rom.bin";
    value.NameList = {"sentinel-map"};
    value.ScalingNameList = {"sentinel-scaling"};
    value.RomInfo = {"sentinel-info"};
    value.OemEcuFile = false;
    value.SyncedWithEcu = false;
    value.use_romraider_definition = true;
    value.use_ecuflash_definition = false;
    const auto original = value;

    auto result = adapter.replace_definition(
        value, *catalog, DefinitionFormat::EcuFlash, "MISSING");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), (Error{ErrorKind::Disconnected, "definition read failed"}));
    EXPECT_EQ(value, original);
}

TEST_F(LegacyDefinitionAdapterTest, CreationAndImportDelegateToDefinitionService)
{
    const DefinitionHeaderInput input{
        .xml_id = "NEW",
        .internal_id = "INTERNAL",
        .ecu_id = "ECU",
        .internal_id_address = 0x20,
    };
    repository.files["source.xml"] =
        bytes("<rom><romid><xmlid>OLD</xmlid></romid><table name=\"Fuel\"/></rom>");

    auto created = adapter.create_definition("created.xml", input);
    auto imported = adapter.import_definition("source.xml", "imported.xml", input);

    ASSERT_TRUE(created);
    ASSERT_TRUE(imported);
    ASSERT_EQ(writer.calls.size(), 2U);
    EXPECT_EQ(writer.calls.at(0).first, "created.xml");
    EXPECT_EQ(writer.calls.at(1).first, "imported.xml");
}

TEST_F(LegacyDefinitionAdapterTest, CreationAndImportPropagateExactServiceFailures)
{
    const DefinitionHeaderInput input{
        .xml_id = "NEW",
        .internal_id = "INTERNAL",
        .ecu_id = "ECU",
        .internal_id_address = 0x20,
    };
    writer.error = Error{ErrorKind::Internal, "atomic replace failed"};

    auto created = adapter.create_definition("created.xml", input);

    ASSERT_FALSE(created);
    EXPECT_EQ(created.error(), (Error{ErrorKind::Internal, "atomic replace failed"}));
    ASSERT_EQ(writer.calls.size(), 1U);
    repository.read_errors["source.xml"] =
        Error{ErrorKind::Disconnected, "source read failed"};

    auto imported = adapter.import_definition("source.xml", "imported.xml", input);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error(), (Error{ErrorKind::Disconnected, "source read failed"}));
    EXPECT_EQ(writer.calls.size(), 1U);
}

} // namespace
} // namespace fastecu::definition
