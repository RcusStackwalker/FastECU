#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"

namespace
{
class CountingFileRepository : public fastecu::IFileRepository
{
  public:
    fastecu::Result<std::vector<std::uint8_t>> read(std::string_view handle) override
    {
        ++readCount;
        ++readCounts[std::string(handle)];
        return repository.read(handle);
    }

    fastecu::Status write(std::string_view handle, std::span<const std::uint8_t> data) override
    {
        return repository.write(handle, data);
    }

    int readCount{0};
    std::map<std::string, int> readCounts;

  private:
    QtFileRepository repository;
};

bool sinkContainsMessage(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level, const QString& text)
{
    return std::ranges::any_of(sink.logs, [&](const auto& entry)
                               { return entry.first == level && QString::fromStdString(entry.second).contains(text); });
}

int logCountAt(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level)
{
    return static_cast<int>(
        std::ranges::count_if(sink.logs, [level](const auto& entry) { return entry.first == level; }));
}

QString firstMessageAt(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level)
{
    const auto it = std::ranges::find(sink.logs, level, [](const auto& entry) { return entry.first; });
    return it == sink.logs.end() ? QString() : QString::fromStdString(it->second);
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

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id << "TESTCAL";
        fileActions.ConfigValuesStruct.ecuflash_def_filename << defPath;

        FileActions::EcuCalDefStructure ecuCalDef;
        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL"), &ecuCalDef);

        QVERIFY2(logCountAt(eventSink, fastecu::LogLevel::Error) == 0,
                 qPrintable(firstMessageAt(eventSink, fastecu::LogLevel::Error)));
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
        const QString defPath = writeDefFile(dir, "TESTCAL",
                                             "<rom><romid><xmlid>TESTCAL</xmlid>"
                                             "<flashmethod>denso_can</flashmethod></romid></rom>");
        QVERIFY(!defPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        auto& config = fileActions.ConfigValuesStruct;
        config.ecuflash_def_cal_id = {"TESTCAL"};
        config.ecuflash_def_filename = {defPath};
        config.flash_protocol_id = {"subaru-denso"};
        config.flash_protocol_alias = {"denso_kline,denso_can"};
        config.flash_protocol_protocol_name = {"sub_ecu_denso_can"};

        FileActions::EcuCalDefStructure ecuCalDef;
        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL"), &ecuCalDef);

        QCOMPARE(ecuCalDef.RomInfo.at(FileActions::FlashMethod), QString("sub_ecu_denso_can"));
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

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
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

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
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

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
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

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
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

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id << "TESTCAL";
        fileActions.ConfigValuesStruct.ecuflash_def_filename << defPath;

        FileActions::EcuCalDefStructure ecuCalDef;
        fileActions.read_ecuflash_ecu_def(&ecuCalDef, "TESTCAL");

        QCOMPARE(ecuCalDef.SwapXYList.at(0), QString("false"));
        QCOMPARE(ecuCalDef.FlipXList.at(0), QString("false"));
        QCOMPARE(ecuCalDef.FlipYList.at(0), QString("false"));
    }

    void inherits_base_table_and_scaling()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString basePath =
            writeDefFile(dir, "BASE_TEST",
                         "<rom>"
                         "<romid><xmlid>BASE_TEST</xmlid></romid>"
                         "<scaling name=\"FuelScale\" units=\"%\" toexpr=\"x*0.5\" frexpr=\"x*2\" "
                         "format=\"%.1f\" min=\"0\" max=\"100\" inc=\"1\" "
                         "storagetype=\"uint16\" endian=\"big\"/>"
                         "<table name=\"Fuel\" address=\"1000\" type=\"1D\" sizex=\"1\" sizey=\"1\" "
                         "scaling=\"FuelScale\"/>"
                         "</rom>");
        QVERIFY(!basePath.isEmpty());
        const QString childPath = writeDefFile(dir, "CHILD_TEST",
                                               "<rom>"
                                               "<romid><xmlid>CHILD_TEST</xmlid><ecuid>TEST_ECU</ecuid></romid>"
                                               "<include>BASE_TEST</include>"
                                               "<table name=\"Fuel\" address=\"2000\"/>"
                                               "</rom>");
        QVERIFY(!childPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id = {"CHILD_TEST", "BASE_TEST"};
        fileActions.ConfigValuesStruct.ecuflash_def_filename = {childPath, basePath};

        FileActions::EcuCalDefStructure ecuCalDef;
        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "CHILD_TEST"), &ecuCalDef);

        QCOMPARE(ecuCalDef.RomInfo.at(FileActions::XmlId), QString("CHILD_TEST"));
        QCOMPARE(ecuCalDef.NameList.at(0), QString("Fuel"));
        QCOMPARE(ecuCalDef.AddressList.at(0), QString("2000"));
        QCOMPARE(ecuCalDef.StorageTypeList.at(0), QString("uint16"));
        QCOMPARE(ecuCalDef.EndianList.at(0), QString("big"));
        QCOMPARE(ecuCalDef.FromByteList.at(0), QString("x*0.5"));
        QCOMPARE(ecuCalDef.ToByteList.at(0), QString("x*2"));
        QCOMPARE(ecuCalDef.FormatList.at(0), QString("0.0"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Debug,
                                    "Definition for CAL ID CHILD_TEST succesfully read"));
    }

    void resolved_partial_scalings_remain_a_full_state_no_io_noop_when_invoked_twice()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeDefFile(dir, "PARTIAL",
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

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        fileActions.ConfigValuesStruct.ecuflash_def_cal_id = {"PARTIAL"};
        fileActions.ConfigValuesStruct.ecuflash_def_filename = {
            definitionPath,
        };
        FileActions::EcuCalDefStructure ecuCalDef;
        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "PARTIAL"), &ecuCalDef);
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
        QCOMPARE(ecuCalDef.ScalingStorageTypeList, QStringList({" ", " ", " "}));
        QCOMPARE(ecuCalDef.ScalingEndianList, QStringList({" ", " ", " "}));
    }

    void malformed_ecuflash_catalog_file_is_skipped_and_replaces_with_empty_catalog()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeDefFile(dir, "BROKEN", "<rom><romid>");
        QVERIFY(!definitionPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        auto& config = fileActions.ConfigValuesStruct;
        config.ecuflash_definition_files_directory = dir.path();
        config.ecuflash_def_cal_id = {"sentinel-id"};
        config.ecuflash_def_cal_id_addr = {"sentinel-address"};
        config.ecuflash_def_ecu_id = {"sentinel-ecu"};
        config.ecuflash_def_filename = {"sentinel-source"};

        QCOMPARE(fileActions.create_ecuflash_def_id_list(&config), &config);

        // A malformed file in the configured directory is skipped rather than treated as
        // fatal (see DefinitionService::build_catalog), so this replace succeeds with an
        // empty catalog instead of preserving the old rows.
        QVERIFY(config.ecuflash_def_cal_id.isEmpty());
        QVERIFY(config.ecuflash_def_cal_id_addr.isEmpty());
        QVERIFY(config.ecuflash_def_ecu_id.isEmpty());
        QVERIFY(config.ecuflash_def_filename.isEmpty());
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 0);
    }

    void removed_configured_definition_is_not_retried_on_refresh()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const QString definitionPath =
            writeDefFileAt(workspace.filePath("removed.xml"), "<rom><romid><xmlid>REMOVED_XML</xmlid>"
                                                              "<internalidaddress>10</internalidaddress>"
                                                              "<internalidstring>REMOVED_INTERNAL</internalidstring>"
                                                              "</romid></rom>");
        QVERIFY(!definitionPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        auto& config = fileActions.ConfigValuesStruct;
        config.ecuflash_definition_files_directory = workspace.path();

        QCOMPARE(fileActions.create_ecuflash_def_id_list(&config), &config);
        QCOMPARE(config.ecuflash_def_cal_id, QStringList({"REMOVED_XML"}));
        QCOMPARE(fileRepository_.readCounts.at(definitionPath.toStdString()), 1);
        QVERIFY(QFile::remove(definitionPath));

        QCOMPARE(fileActions.create_ecuflash_def_id_list(&config), &config);

        QVERIFY(logCountAt(eventSink, fastecu::LogLevel::Error) == 0);
        QVERIFY(config.ecuflash_def_cal_id.isEmpty());
        QVERIFY(config.ecuflash_def_cal_id_addr.isEmpty());
        QVERIFY(config.ecuflash_def_ecu_id.isEmpty());
        QVERIFY(config.ecuflash_def_filename.isEmpty());
        QCOMPARE(fileRepository_.readCounts.at(definitionPath.toStdString()), 1);
    }

    void malformed_ecuflash_definition_reports_definition_not_found()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeDefFile(dir, "BROKEN", "<rom><romid>");
        QVERIFY(!definitionPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        auto& config = fileActions.ConfigValuesStruct;
        config.ecuflash_def_cal_id = {"BROKEN"};
        config.ecuflash_def_cal_id_addr = {"0"};
        config.ecuflash_def_ecu_id = {"sentinel-ecu"};
        config.ecuflash_def_filename = {definitionPath};

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.RomInfo = QStringList(ecuCalDef.RomInfoStrings.size(), "sentinel-rom-info");
        ecuCalDef.DefinitionFileName = "sentinel-definition-file";
        ecuCalDef.NameList = {"sentinel-map"};
        ecuCalDef.use_ecuflash_definition = false;
        const QStringList romInfo = ecuCalDef.RomInfo;
        const QString definitionFileName = ecuCalDef.DefinitionFileName;
        const QStringList names = ecuCalDef.NameList;

        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "BROKEN"), &ecuCalDef);

        QCOMPARE(ecuCalDef.RomInfo, romInfo);
        QCOMPARE(ecuCalDef.DefinitionFileName, definitionFileName);
        QCOMPARE(ecuCalDef.NameList, names);
        QVERIFY(!ecuCalDef.use_ecuflash_definition);
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        // The malformed file is skipped while building the catalog (see
        // DefinitionService::build_catalog), so "BROKEN" is simply absent from it rather than
        // failing with a parse error.
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "definition ID not found"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "BROKEN"));
    }

    void missing_ecuflash_definition_reports_definition_not_found()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        auto& config = fileActions.ConfigValuesStruct;
        config.ecuflash_def_cal_id = {"MISSING"};
        config.ecuflash_def_cal_id_addr = {"0"};
        config.ecuflash_def_ecu_id = {"sentinel-ecu"};
        config.ecuflash_def_filename = {dir.filePath("missing.xml")};

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.RomInfo = QStringList(ecuCalDef.RomInfoStrings.size(), "sentinel-rom-info");
        ecuCalDef.DefinitionFileName = "sentinel-definition-file";
        ecuCalDef.NameList = {"sentinel-map"};
        const QStringList romInfo = ecuCalDef.RomInfo;
        const QString definitionFileName = ecuCalDef.DefinitionFileName;
        const QStringList names = ecuCalDef.NameList;

        // Unlike read_romraider_ecu_base_def's single required base file, this rebuilds the
        // catalog from every currently known EcuFlash file (see build_definition_catalog), so
        // the unreadable entry is skipped rather than failing the whole lookup -- "MISSING" is
        // then simply absent from the resulting (empty) catalog. The configured source file
        // still doesn't exist on disk though, so this still takes the "missing file" branch
        // (a notice, nullptr) rather than the "found but broken" branch.
        QCOMPARE(fileActions.read_ecuflash_ecu_def(&ecuCalDef, "MISSING"), nullptr);

        QCOMPARE(ecuCalDef.RomInfo, romInfo);
        QCOMPARE(ecuCalDef.DefinitionFileName, definitionFileName);
        QCOMPARE(ecuCalDef.NameList, names);
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "definition ID not found"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "MISSING"));
        QVERIFY(!eventSink.notices.empty());
        QVERIFY(QString::fromStdString(eventSink.notices.front()).contains("Unable to open ECU definition file"));
    }

    void ecuflash_rom_match_failure_preserves_rom_id_and_logs_error()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeDefFile(dir, "TESTCAL",
                                                    "<rom><romid><xmlid>TESTCAL</xmlid>"
                                                    "<internalidaddress>0</internalidaddress>"
                                                    "<internalidstring>TESTCAL</internalidstring></romid></rom>");
        QVERIFY(!definitionPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        fileActions.ConfigValuesStruct.ecuflash_definition_files_directory = dir.path();
        QCOMPARE(fileActions.create_ecuflash_def_id_list(&fileActions.ConfigValuesStruct),
                 &fileActions.ConfigValuesStruct);

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.RomId = "sentinel-rom-id";
        ecuCalDef.FullRomData = QByteArray("NO_MATCH");

        QCOMPARE(fileActions.parse_ecuid_ecuflash_def_files(&ecuCalDef, true), &ecuCalDef);

        QCOMPARE(ecuCalDef.RomId, QString("sentinel-rom-id"));
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "no matching ROM definition"));
    }

  private:
    static QString writeDefFileAt(const QString& path, const QString& xml)
    {
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

    static QString writeDefFile(const QTemporaryDir& dir, const QString& baseName, const QString& xml)
    {
        const QString path = dir.filePath(baseName + ".xml");
        return writeDefFileAt(path, xml);
    }

    // FileActions's constructor now takes the config/settings ports (Task
    // 11 of the step5d-1 plan); these are unused by the parsing paths this
    // test exercises, so plain default-constructed Qt port implementations
    // are sufficient.
    QtFileSystem fileSystem_;
    QtResourceBundle resourceBundle_;
    CountingFileRepository fileRepository_;
    fastecu::InMemoryAtomicFileWriter atomicFileWriter_;
};

QTEST_APPLESS_MAIN(TestEcuflashDefinitionParsing)
#include "ecuflash_definition_parsing_test.moc"
