#include <QtTest>
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QSignalSpy>
#include <QTimer>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/ports/atomic_file_writer.h"
#include "src/backend/definitions/file_actions.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "test_ecuflash_definition_parsing.h"

namespace
{
class InMemoryAtomicFileWriter : public fastecu::IAtomicFileWriter
{
  public:
    fastecu::Status replace(std::string_view handle,
                            std::span<const std::uint8_t> data) override
    {
        files[std::string(handle)] =
            std::vector<std::uint8_t>(data.begin(), data.end());
        return {};
    }

    std::map<std::string, std::vector<std::uint8_t>> files;
};

class CountingFileRepository : public fastecu::IFileRepository
{
  public:
    fastecu::Result<std::vector<std::uint8_t>> read(
        std::string_view handle) override
    {
        ++readCount;
        return repository.read(handle);
    }

    fastecu::Status write(
        std::string_view handle,
        std::span<const std::uint8_t> data) override
    {
        return repository.write(handle, data);
    }

    int readCount{0};

  private:
    QtFileRepository repository;
};

bool spyContainsMessage(const QSignalSpy& spy, const QString& text)
{
    for (const QList<QVariant>& arguments : spy)
    {
        if (!arguments.isEmpty() && arguments.at(0).toString().contains(text))
        {
            return true;
        }
    }
    return false;
}
} // namespace

class TestEcuflashDefinitionParsing : public QObject
{
    Q_OBJECT
  private slots:
    void parses_subcategory_level_userlevel_description()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString defPath = writeDefFile(dir, "TESTCAL",
                                             "<rom>"
                                             "<romid><xmlid>TESTCAL</xmlid></romid>"
                                             "<table name=\"Test Table\" address=\"1000\" category=\"Fuel\" "
                                             "subcategory=\"Primary\" level=\"2\" userlevel=\"3\" "
                                             "description=\"A test table\"/>"
                                             "</rom>");
        QVERIFY(!defPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id << "TESTCAL";
        fileActions.ConfigValuesStruct.ecuflash_def_filename << defPath;
        QSignalSpy errorSpy(&fileActions, &FileActions::LOG_E);

        FileActions::EcuCalDefStructure ecuCalDef;
        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL"),
                 &ecuCalDef);

        QVERIFY2(
            errorSpy.isEmpty(),
            errorSpy.isEmpty()
                ? ""
                : qPrintable(errorSpy.at(0).at(0).toString()));
        QCOMPARE(ecuCalDef.NameList.size(), 1);
        QCOMPARE(ecuCalDef.SubCategoryList.at(0), QString("Primary"));
        QCOMPARE(ecuCalDef.LevelList.at(0), QString("2"));
        QCOMPARE(ecuCalDef.UserLevelList.at(0), QString("3"));
        QCOMPARE(ecuCalDef.DescriptionList.at(0), QString("A test table"));
    }

    void successful_read_translates_flash_method_alias_to_protocol_name()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString defPath = writeDefFile(
            dir,
            "TESTCAL",
            "<rom><romid><xmlid>TESTCAL</xmlid>"
            "<flashmethod>denso_can</flashmethod></romid></rom>");
        QVERIFY(!defPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = fileActions.ConfigValuesStruct;
        config.ecuflash_def_cal_id = {"TESTCAL"};
        config.ecuflash_def_filename = {defPath};
        config.flash_protocol_id = {"subaru-denso"};
        config.flash_protocol_alias = {"denso_kline,denso_can"};
        config.flash_protocol_protocol_name = {"sub_ecu_denso_can"};

        FileActions::EcuCalDefStructure ecuCalDef;
        QCOMPARE(
            fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL"),
            &ecuCalDef);

        QCOMPARE(
            ecuCalDef.RomInfo.at(FileActions::FlashMethod),
            QString("sub_ecu_denso_can"));
    }

    void storageaddress_populates_address_when_address_absent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString defPath = writeDefFile(dir, "TESTCAL",
                                             "<rom>"
                                             "<romid><xmlid>TESTCAL</xmlid></romid>"
                                             "<table name=\"Test Table\" storageaddress=\"2000\"/>"
                                             "</rom>");
        QVERIFY(!defPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id << "TESTCAL";
        fileActions.ConfigValuesStruct.ecuflash_def_filename << defPath;

        FileActions::EcuCalDefStructure ecuCalDef;
        fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL");

        QCOMPARE(ecuCalDef.AddressList.at(0), QString("2000"));
    }

    void address_wins_over_storageaddress_when_both_present()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString defPath = writeDefFile(dir, "TESTCAL",
                                             "<rom>"
                                             "<romid><xmlid>TESTCAL</xmlid></romid>"
                                             "<table name=\"Test Table\" address=\"1000\" storageaddress=\"2000\"/>"
                                             "</rom>");
        QVERIFY(!defPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id << "TESTCAL";
        fileActions.ConfigValuesStruct.ecuflash_def_filename << defPath;

        FileActions::EcuCalDefStructure ecuCalDef;
        fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL");

        QCOMPARE(ecuCalDef.AddressList.at(0), QString("1000"));
    }

    void outer_table_sizex_sizey_populate_x_y_size_lists()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString defPath = writeDefFile(dir, "TESTCAL",
                                             "<rom>"
                                             "<romid><xmlid>TESTCAL</xmlid></romid>"
                                             "<table name=\"Test Table\" address=\"1000\" sizex=\"12\" sizey=\"8\"/>"
                                             "</rom>");
        QVERIFY(!defPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id << "TESTCAL";
        fileActions.ConfigValuesStruct.ecuflash_def_filename << defPath;

        FileActions::EcuCalDefStructure ecuCalDef;
        fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL");

        QCOMPARE(ecuCalDef.XSizeList.at(0), QString("12"));
        QCOMPARE(ecuCalDef.YSizeList.at(0), QString("8"));
    }

    void parses_swapxy_flipx_flipy_when_valid()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString defPath = writeDefFile(dir, "TESTCAL",
                                             "<rom>"
                                             "<romid><xmlid>TESTCAL</xmlid></romid>"
                                             "<table name=\"Test Table\" address=\"1000\" "
                                             "swapxy=\"true\" flipx=\"false\" flipy=\"true\"/>"
                                             "</rom>");
        QVERIFY(!defPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id << "TESTCAL";
        fileActions.ConfigValuesStruct.ecuflash_def_filename << defPath;

        FileActions::EcuCalDefStructure ecuCalDef;
        fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL");

        QCOMPARE(ecuCalDef.SwapXYList.at(0), QString("true"));
        QCOMPARE(ecuCalDef.FlipXList.at(0), QString("false"));
        QCOMPARE(ecuCalDef.FlipYList.at(0), QString("true"));
    }

    void missing_swapxy_flipx_flipy_default_to_false()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString defPath = writeDefFile(dir, "TESTCAL",
                                             "<rom>"
                                             "<romid><xmlid>TESTCAL</xmlid></romid>"
                                             "<table name=\"Test Table\" address=\"1000\"/>"
                                             "</rom>");
        QVERIFY(!defPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id << "TESTCAL";
        fileActions.ConfigValuesStruct.ecuflash_def_filename << defPath;

        FileActions::EcuCalDefStructure ecuCalDef;
        fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL");

        QCOMPARE(ecuCalDef.SwapXYList.at(0), QString("false"));
        QCOMPARE(ecuCalDef.FlipXList.at(0), QString("false"));
        QCOMPARE(ecuCalDef.FlipYList.at(0), QString("false"));
    }

    void invalid_swapxy_value_preserves_caller_state_and_logs_error()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString defPath = writeDefFile(dir, "TESTCAL",
                                             "<rom>"
                                             "<romid><xmlid>TESTCAL</xmlid></romid>"
                                             "<table name=\"Test Table\" address=\"1000\" swapxy=\"yes\"/>"
                                             "</rom>");
        QVERIFY(!defPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id << "TESTCAL";
        fileActions.ConfigValuesStruct.ecuflash_def_filename << defPath;

        QSignalSpy errorSpy(&fileActions, &FileActions::LOG_E);

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.RomInfo =
            QStringList(ecuCalDef.RomInfoStrings.size(), "sentinel-rom-info");
        ecuCalDef.NameList = {"sentinel-map"};
        ecuCalDef.SwapXYList = {"sentinel-swap"};
        const QStringList romInfo = ecuCalDef.RomInfo;
        const QStringList names = ecuCalDef.NameList;
        const QStringList swapXY = ecuCalDef.SwapXYList;
        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL"),
                 &ecuCalDef);

        QCOMPARE(ecuCalDef.RomInfo, romInfo);
        QCOMPARE(ecuCalDef.NameList, names);
        QCOMPARE(ecuCalDef.SwapXYList, swapXY);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "swapxy"));
        QVERIFY(spyContainsMessage(errorSpy, "yes"));
    }

    void inherits_base_table_and_scaling()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString basePath = writeDefFile(
            dir,
            "BASE_TEST",
            "<rom>"
            "<romid><xmlid>BASE_TEST</xmlid></romid>"
            "<scaling name=\"FuelScale\" units=\"%\" toexpr=\"x*0.5\" frexpr=\"x*2\" "
            "format=\"%.1f\" min=\"0\" max=\"100\" inc=\"1\" "
            "storagetype=\"uint16\" endian=\"big\"/>"
            "<table name=\"Fuel\" address=\"1000\" type=\"1D\" sizex=\"1\" sizey=\"1\" "
            "scaling=\"FuelScale\"/>"
            "</rom>");
        QVERIFY(!basePath.isEmpty());
        const QString childPath = writeDefFile(
            dir,
            "CHILD_TEST",
            "<rom>"
            "<romid><xmlid>CHILD_TEST</xmlid><ecuid>TEST_ECU</ecuid></romid>"
            "<include>BASE_TEST</include>"
            "<table name=\"Fuel\" address=\"2000\"/>"
            "</rom>");
        QVERIFY(!childPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        QSignalSpy debugSpy(&fileActions, &FileActions::LOG_D);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id = {"CHILD_TEST", "BASE_TEST"};
        fileActions.ConfigValuesStruct.ecuflash_def_filename = {childPath, basePath};

        FileActions::EcuCalDefStructure ecuCalDef;
        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "CHILD_TEST"), &ecuCalDef);
        const int readsAfterDefinition = fileRepository_.readCount;
        const FileActions::EcuCalDefStructure resolved = ecuCalDef;
        QCOMPARE(fileActions.parse_ecuflash_def_scalings(&ecuCalDef), &ecuCalDef);
        QCOMPARE(fileRepository_.readCount, readsAfterDefinition);
        QVERIFY(ecuCalDef == resolved);
        QCOMPARE(fileActions.parse_ecuflash_def_scalings(&ecuCalDef), &ecuCalDef);
        QCOMPARE(fileRepository_.readCount, readsAfterDefinition);
        QVERIFY(ecuCalDef == resolved);

        QCOMPARE(ecuCalDef.RomInfo.at(FileActions::XmlId), QString("CHILD_TEST"));
        QCOMPARE(ecuCalDef.NameList.at(0), QString("Fuel"));
        QCOMPARE(ecuCalDef.AddressList.at(0), QString("2000"));
        QCOMPARE(ecuCalDef.StorageTypeList.at(0), QString("uint16"));
        QCOMPARE(ecuCalDef.EndianList.at(0), QString("big"));
        QCOMPARE(ecuCalDef.FromByteList.at(0), QString("x*0.5"));
        QCOMPARE(ecuCalDef.ToByteList.at(0), QString("x*2"));
        QCOMPARE(ecuCalDef.FormatList.at(0), QString("0.0"));
        QVERIFY(spyContainsMessage(
            debugSpy,
            "Definition for CAL ID CHILD_TEST succesfully read"));
    }

    void resolved_partial_scalings_remain_a_full_state_no_io_noop_when_invoked_twice()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeDefFile(
            dir,
            "PARTIAL",
            R"xml(<rom>
  <romid><xmlid>PARTIAL</xmlid></romid>
  <scaling name="MapScale" units="%" toexpr="x*2" frexpr="x/2" format="%.1f"/>
  <scaling name="XScale" units="load" toexpr="x+1" frexpr="x-1" format="%.2f"/>
  <scaling name="YScale" units="rpm" toexpr="x*4" frexpr="x/4" format="%.0f"/>
  <table name="Fuel" type="3D" sizex="2" sizey="2"
         scaling="MapScale" storagetype="uint16" endian="big">
    <table name="Load" type="X Axis" elements="2"
           scaling="XScale" storagetype="uint8" endian="little"/>
    <table name="RPM" type="Y Axis" elements="2"
           scaling="YScale" storagetype="uint16" endian="big"/>
  </table>
</rom>)xml");
        QVERIFY(!definitionPath.isEmpty());

        FileActions fileActions(
            fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id = {"PARTIAL"};
        fileActions.ConfigValuesStruct.ecuflash_def_filename = {
            definitionPath,
        };
        FileActions::EcuCalDefStructure ecuCalDef;
        QCOMPARE(
            fileActions.read_ecuflash_ecu_def(&ecuCalDef, "PARTIAL"),
            &ecuCalDef);
        QVERIFY(ecuCalDef.use_ecuflash_definition);
        QCOMPARE(ecuCalDef.StorageTypeList.at(0), QString("uint16"));
        QCOMPARE(ecuCalDef.EndianList.at(0), QString("big"));
        QCOMPARE(ecuCalDef.FromByteList.at(0), QString("x*2"));
        QCOMPARE(ecuCalDef.ToByteList.at(0), QString("x/2"));
        QCOMPARE(ecuCalDef.XScaleStorageTypeList.at(0), QString("uint8"));
        QCOMPARE(ecuCalDef.XScaleEndianList.at(0), QString("little"));
        QCOMPARE(ecuCalDef.XScaleFromByteList.at(0), QString("x+1"));
        QCOMPARE(ecuCalDef.XScaleToByteList.at(0), QString("x-1"));
        QCOMPARE(ecuCalDef.YScaleStorageTypeList.at(0), QString("uint16"));
        QCOMPARE(ecuCalDef.YScaleEndianList.at(0), QString("big"));
        QCOMPARE(ecuCalDef.YScaleFromByteList.at(0), QString("x*4"));
        QCOMPARE(ecuCalDef.YScaleToByteList.at(0), QString("x/4"));
        QCOMPARE(
            ecuCalDef.ScalingStorageTypeList,
            QStringList({" ", " ", " "}));
        QCOMPARE(
            ecuCalDef.ScalingEndianList,
            QStringList({" ", " ", " "}));
        const int readsAfterDefinition = fileRepository_.readCount;
        const FileActions::EcuCalDefStructure resolved = ecuCalDef;

        QCOMPARE(
            fileActions.parse_ecuflash_def_scalings(&ecuCalDef),
            &ecuCalDef);
        QCOMPARE(fileRepository_.readCount, readsAfterDefinition);
        QVERIFY(ecuCalDef == resolved);
        QCOMPARE(
            fileActions.parse_ecuflash_def_scalings(&ecuCalDef),
            &ecuCalDef);
        QCOMPARE(fileRepository_.readCount, readsAfterDefinition);
        QVERIFY(ecuCalDef == resolved);
    }

    void standalone_scaling_parse_uses_prepopulated_rows_without_file_io()
    {
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.NameList = {"Fuel"};
        ecuCalDef.MapScalingNameList = {"FuelScale"};
        ecuCalDef.TypeList = {" "};
        ecuCalDef.StorageTypeList = {" "};
        ecuCalDef.UnitsList = {" "};
        ecuCalDef.FineIncList = {" "};
        ecuCalDef.CoarseIncList = {" "};
        ecuCalDef.MinValueList = {" "};
        ecuCalDef.MaxValueList = {" "};
        ecuCalDef.EndianList = {" "};
        ecuCalDef.FromByteList = {" "};
        ecuCalDef.ToByteList = {" "};
        ecuCalDef.FormatList = {" "};
        ecuCalDef.SelectionsNameList = {" "};
        ecuCalDef.SelectionsValueList = {" "};
        ecuCalDef.XScaleScalingNameList = {" "};
        ecuCalDef.YScaleScalingNameList = {" "};
        ecuCalDef.ScalingNameList = {"FuelScale"};
        ecuCalDef.ScalingUnitsList = {"%"};
        ecuCalDef.ScalingFromByteList = {"x*0.5"};
        ecuCalDef.ScalingToByteList = {"x*2"};
        ecuCalDef.ScalingFormatList = {"%.2f"};
        ecuCalDef.ScalingMinValueList = {"0"};
        ecuCalDef.ScalingMaxValueList = {"100"};
        ecuCalDef.ScalingCoarseIncList = {"1"};
        ecuCalDef.ScalingFineIncList = {"0.1"};
        ecuCalDef.ScalingStorageTypeList = {"bloblist"};
        ecuCalDef.ScalingEndianList = {"big"};
        ecuCalDef.ScalingSelectionsNameList = {"disabled,enabled,"};
        ecuCalDef.ScalingSelectionsValueList = {"00,01,"};
        const int readsBeforeScaling = fileRepository_.readCount;

        QCOMPARE(
            fileActions.parse_ecuflash_def_scalings(&ecuCalDef),
            &ecuCalDef);

        QCOMPARE(fileRepository_.readCount, readsBeforeScaling);
        QCOMPARE(ecuCalDef.TypeList.at(0), QString("Selectable"));
        QCOMPARE(ecuCalDef.StorageTypeList.at(0), QString("bloblist"));
        QCOMPARE(ecuCalDef.UnitsList.at(0), QString("%"));
        QCOMPARE(ecuCalDef.FineIncList.at(0), QString("0.1"));
        QCOMPARE(ecuCalDef.CoarseIncList.at(0), QString("1"));
        QCOMPARE(ecuCalDef.MinValueList.at(0), QString("0"));
        QCOMPARE(ecuCalDef.MaxValueList.at(0), QString("100"));
        QCOMPARE(ecuCalDef.EndianList.at(0), QString("big"));
        QCOMPARE(ecuCalDef.FromByteList.at(0), QString("x*0.5"));
        QCOMPARE(ecuCalDef.ToByteList.at(0), QString("x*2"));
        QCOMPARE(ecuCalDef.FormatList.at(0), QString("0.00"));
        QCOMPARE(
            ecuCalDef.SelectionsNameList.at(0),
            QString("disabled,enabled,"));
        QCOMPARE(
            ecuCalDef.SelectionsValueList.at(0),
            QString("00,01,"));

        const FileActions::EcuCalDefStructure parsed = ecuCalDef;
        QCOMPARE(
            fileActions.parse_ecuflash_def_scalings(&ecuCalDef),
            &ecuCalDef);
        QCOMPARE(fileRepository_.readCount, readsBeforeScaling);
        QVERIFY(ecuCalDef == parsed);
    }

    void malformed_ecuflash_catalog_preserves_existing_rows_and_logs_error()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath =
            writeDefFile(dir, "BROKEN", "<rom><romid>");
        QVERIFY(!definitionPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = fileActions.ConfigValuesStruct;
        config.ecuflash_definition_files_directory = dir.path();
        config.ecuflash_def_cal_id = {"sentinel-id"};
        config.ecuflash_def_cal_id_addr = {"sentinel-address"};
        config.ecuflash_def_ecu_id = {"sentinel-ecu"};
        config.ecuflash_def_filename = {"sentinel-source"};
        const QStringList ids = config.ecuflash_def_cal_id;
        const QStringList addresses = config.ecuflash_def_cal_id_addr;
        const QStringList ecuIds = config.ecuflash_def_ecu_id;
        const QStringList sources = config.ecuflash_def_filename;
        QSignalSpy errorSpy(&fileActions, &FileActions::LOG_E);

        QCOMPARE(fileActions.create_ecuflash_def_id_list(&config), &config);

        QCOMPARE(config.ecuflash_def_cal_id, ids);
        QCOMPARE(config.ecuflash_def_cal_id_addr, addresses);
        QCOMPARE(config.ecuflash_def_ecu_id, ecuIds);
        QCOMPARE(config.ecuflash_def_filename, sources);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "malformed XML"));
    }

    void malformed_ecuflash_definition_preserves_caller_state()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath =
            writeDefFile(dir, "BROKEN", "<rom><romid>");
        QVERIFY(!definitionPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = fileActions.ConfigValuesStruct;
        config.ecuflash_def_cal_id = {"BROKEN"};
        config.ecuflash_def_cal_id_addr = {"0"};
        config.ecuflash_def_ecu_id = {"sentinel-ecu"};
        config.ecuflash_def_filename = {definitionPath};

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.RomInfo =
            QStringList(ecuCalDef.RomInfoStrings.size(), "sentinel-rom-info");
        ecuCalDef.DefinitionFileName = "sentinel-definition-file";
        ecuCalDef.NameList = {"sentinel-map"};
        ecuCalDef.use_ecuflash_definition = false;
        const QStringList romInfo = ecuCalDef.RomInfo;
        const QString definitionFileName = ecuCalDef.DefinitionFileName;
        const QStringList names = ecuCalDef.NameList;
        QSignalSpy errorSpy(&fileActions, &FileActions::LOG_E);

        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "BROKEN"),
                 &ecuCalDef);

        QCOMPARE(ecuCalDef.RomInfo, romInfo);
        QCOMPARE(ecuCalDef.DefinitionFileName, definitionFileName);
        QCOMPARE(ecuCalDef.NameList, names);
        QVERIFY(!ecuCalDef.use_ecuflash_definition);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "malformed XML"));
    }

    void missing_ecuflash_definition_returns_null_and_preserves_caller_state()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = fileActions.ConfigValuesStruct;
        config.ecuflash_def_cal_id = {"MISSING"};
        config.ecuflash_def_cal_id_addr = {"0"};
        config.ecuflash_def_ecu_id = {"sentinel-ecu"};
        config.ecuflash_def_filename = {dir.filePath("missing.xml")};

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.RomInfo =
            QStringList(ecuCalDef.RomInfoStrings.size(), "sentinel-rom-info");
        ecuCalDef.DefinitionFileName = "sentinel-definition-file";
        ecuCalDef.NameList = {"sentinel-map"};
        const QStringList romInfo = ecuCalDef.RomInfo;
        const QString definitionFileName = ecuCalDef.DefinitionFileName;
        const QStringList names = ecuCalDef.NameList;
        QSignalSpy errorSpy(&fileActions, &FileActions::LOG_E);
        QTimer::singleShot(0, []()
                           {
            if (QWidget *modal = QApplication::activeModalWidget())
            {
                modal->close();
            } });

        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "MISSING"),
                 nullptr);

        QCOMPARE(ecuCalDef.RomInfo, romInfo);
        QCOMPARE(ecuCalDef.DefinitionFileName, definitionFileName);
        QCOMPARE(ecuCalDef.NameList, names);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "cannot open file"));
    }

    void ecuflash_rom_match_failure_preserves_rom_id_and_logs_error()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeDefFile(
            dir,
            "TESTCAL",
            "<rom><romid><xmlid>TESTCAL</xmlid>"
            "<internalidaddress>0</internalidaddress>"
            "<internalidstring>TESTCAL</internalidstring></romid></rom>");
        QVERIFY(!definitionPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        fileActions.ConfigValuesStruct.ecuflash_definition_files_directory =
            dir.path();
        QCOMPARE(
            fileActions.create_ecuflash_def_id_list(
                &fileActions.ConfigValuesStruct),
            &fileActions.ConfigValuesStruct);

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.RomId = "sentinel-rom-id";
        ecuCalDef.FullRomData = QByteArray("NO_MATCH");
        QSignalSpy errorSpy(&fileActions, &FileActions::LOG_E);

        QCOMPARE(
            fileActions.parse_ecuid_ecuflash_def_files(&ecuCalDef, true),
            &ecuCalDef);

        QCOMPARE(ecuCalDef.RomId, QString("sentinel-rom-id"));
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "no matching ROM definition"));
    }

    void ecuflash_rom_match_accepts_legacy_hex_identifier()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeDefFile(
            dir,
            "AB10",
            "<rom><romid><xmlid>AB10</xmlid>"
            "<internalidaddress>0</internalidaddress>"
            "<internalidstring>AB10</internalidstring></romid></rom>");
        QVERIFY(!definitionPath.isEmpty());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = fileActions.ConfigValuesStruct;
        config.ecuflash_definition_files_directory = dir.path();

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray::fromHex("AB10");
        QSignalSpy errorSpy(&fileActions, &FileActions::LOG_E);

        QCOMPARE(
            fileActions.parse_ecuid_ecuflash_def_files(&ecuCalDef, false),
            &ecuCalDef);

        QCOMPARE(ecuCalDef.RomId, QString("AB10"));
        QVERIFY(errorSpy.isEmpty());
    }

  private:
    static QString writeDefFile(const QTemporaryDir& dir, const QString& baseName, const QString& xml)
    {
        const QString path = dir.filePath(baseName + ".xml");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            return {};
        }
        const QByteArray contents = xml.toUtf8();
        if (file.write(contents) != contents.size())
        {
            return {};
        }
        file.close();
        return path;
    }

    // FileActions's constructor now takes the config/settings ports (Task
    // 11 of the step5d-1 plan); these are unused by the parsing paths this
    // test exercises, so plain default-constructed Qt port implementations
    // are sufficient.
    QtFileSystem fileSystem_;
    QtResourceBundle resourceBundle_;
    CountingFileRepository fileRepository_;
    InMemoryAtomicFileWriter atomicFileWriter_;
};

int run_test_ecuflash_definition_parsing(int argc, char **argv)
{
    // FileActions derives from QWidget (used only for its Q_OBJECT signals/config
    // state here, never shown), which requires a QApplication rather than a plain
    // QCoreApplication to construct.
    QApplication app(argc, argv);
    TestEcuflashDefinitionParsing t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_ecuflash_definition_parsing.moc"
