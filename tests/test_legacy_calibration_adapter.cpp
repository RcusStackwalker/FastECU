#include <QtTest>

#include "src/backend/calibration/legacy_calibration_adapter.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

using fastecu::InMemoryFileRepository;
using fastecu::calibration::LegacyCalibrationAdapter;
using fastecu::definitions::ConfigValuesStructure;
using fastecu::definitions::EcuCalDefStructure;

namespace
{
QByteArray sampleXmlProtocolsFile()
{
    // Wrapped in <config> because load_protocol_catalog/load_car_model_catalog
    // (Task 3, src/backend/config/protocol_catalog.cpp,
    // src/backend/config/car_model_catalog.cpp) look up
    // doc.child("config").child("protocols") /
    // doc.child("config").child("car_models") -- matching real
    // resources/shared/config/protocols.cfg's <config name="..."
    // version="...">-rooted shape. A fixture with <protocols>/<car_models>
    // as document-level siblings (no <config> root) parses to two empty
    // catalogs under that lookup, so bind_protocol would never find a match.
    //
    // <car_model>'s make/model/version are child elements, not attributes:
    // car_model_catalog.cpp's text_or_empty(car_model, "make") reads
    // car_model.child("make").text(), matching test_legacy_config_adapter.cpp's
    // own car_model fixtures (e.g. ReadProtocolsFileJoinsCarModelWithMatchingProtocol).
    return QByteArray(
        "<config>"
        "<protocols>"
        "<protocol name=\"sub_ecu_denso_can\" alias=\"denso_can\">"
        "<mcu>SH7058</mcu><checksum>yes</checksum><mode>can</mode>"
        "</protocol>"
        "</protocols>"
        "<car_models>"
        "<car_model>"
        "<make>Mitsubishi</make><model>Colt</model><version>Z27AG</version>"
        "<protocol>sub_ecu_denso_can</protocol>"
        "</car_model>"
        "</car_models>"
        "</config>");
}
} // namespace

class TestLegacyCalibrationAdapter : public QObject
{
    Q_OBJECT
  private slots:
    void open_rom_bytes_reads_from_disk_when_no_bytes_preloaded()
    {
        InMemoryFileRepository repo;
        repo.files["rom.bin"] = {0x01, 0x02, 0x03};
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ConfigValuesStructure configValues;

        fastecu::Status result = adapter.open_rom_bytes(ecuCalDef, "rom.bin", configValues);

        QVERIFY(result.has_value());
        QCOMPARE(ecuCalDef.FullRomData, QByteArray("\x01\x02\x03", 3));
        QCOMPARE(ecuCalDef.FileName, QString("rom.bin"));
        QCOMPARE(ecuCalDef.FullFileName, QString("rom.bin"));
    }

    void open_rom_bytes_backs_up_already_loaded_bytes()
    {
        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray("\x0A\x0B", 2);
        ConfigValuesStructure configValues;
        configValues.calibration_files_directory = "cal/";

        fastecu::Status result = adapter.open_rom_bytes(ecuCalDef, "", configValues);

        QVERIFY(result.has_value());
        QCOMPARE(ecuCalDef.FullRomData, QByteArray("\x0A\x0B", 2));
        QVERIFY(repo.files.count("cal/read.bin"));
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(repo.files["cal/read.bin"].data()),
                            static_cast<qsizetype>(repo.files["cal/read.bin"].size())),
                 QByteArray("\x0A\x0B", 2));
        QVERIFY(!ecuCalDef.FileName.isEmpty());
    }

    void open_rom_bytes_fails_when_no_filename_and_no_preloaded_bytes()
    {
        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ConfigValuesStructure configValues;

        fastecu::Status result = adapter.open_rom_bytes(ecuCalDef, "", configValues);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, fastecu::ErrorKind::InvalidConfig);
    }

    void save_subaru_rom_file_writes_bytes_and_updates_names()
    {
        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray("\x01\x02", 2);

        EcuCalDefStructure *result =
            adapter.save_subaru_rom_file(&ecuCalDef, "saved/rom.bin");

        QVERIFY(result != nullptr);
        QCOMPARE(ecuCalDef.FullFileName, QString("saved/rom.bin"));
        QCOMPARE(ecuCalDef.FileName, QString("rom.bin"));
        QVERIFY(repo.files.count("saved/rom.bin"));
    }

    void bind_protocol_sets_the_nine_legacy_scalars_on_a_match()
    {
        InMemoryFileRepository repo;
        const QByteArray protocolsXml = sampleXmlProtocolsFile();
        repo.files["config/protocols.cfg"] =
            std::vector<std::uint8_t>(protocolsXml.begin(), protocolsXml.end());
        LegacyCalibrationAdapter adapter(repo);
        ConfigValuesStructure configValues;
        configValues.protocols_file = "config/protocols.cfg";

        adapter.bind_protocol(configValues, "sub_ecu_denso_can");

        QCOMPARE(configValues.flash_protocol_selected_id, QString("0"));
        QCOMPARE(configValues.flash_protocol_selected_make, QString("Mitsubishi"));
        QCOMPARE(configValues.flash_protocol_selected_model, QString("Colt"));
        QCOMPARE(configValues.flash_protocol_selected_version, QString("Z27AG"));
        QCOMPARE(configValues.flash_protocol_selected_protocol_name, QString("sub_ecu_denso_can"));
        QCOMPARE(configValues.flash_protocol_selected_mcu, QString("SH7058"));
        QCOMPARE(configValues.flash_protocol_selected_checksum, QString("yes"));
    }

    void bind_protocol_leaves_selected_fields_untouched_on_no_match()
    {
        InMemoryFileRepository repo;
        const QByteArray protocolsXml = sampleXmlProtocolsFile();
        repo.files["config/protocols.cfg"] =
            std::vector<std::uint8_t>(protocolsXml.begin(), protocolsXml.end());
        LegacyCalibrationAdapter adapter(repo);
        ConfigValuesStructure configValues;
        configValues.protocols_file = "config/protocols.cfg";
        configValues.flash_protocol_selected_mcu = "PRE_EXISTING";

        adapter.bind_protocol(configValues, "no_such_flash_method");

        QCOMPARE(configValues.flash_protocol_selected_mcu, QString("PRE_EXISTING"));
    }

    void compute_map_cell_values_writes_decoded_values_at_matching_index()
    {
        fastecu::definition::RomDefinition definition;
        fastecu::definition::Scaling scaling;
        scaling.name = "s";
        scaling.from_byte = "x*0.5";
        definition.scalings.push_back(scaling);
        fastecu::definition::CalibrationMap map;
        map.name = "Fuel";
        map.address = 10;
        map.x_size = 1;
        map.y_size = 1;
        map.storage_type = "uint16";
        map.endian = "big";
        map.start_position = "1";
        map.interval = "1";
        map.scaling_name = "s";
        definition.maps.push_back(map);

        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray(20, '\0');
        ecuCalDef.FullRomData[10] = static_cast<char>(0x00);
        ecuCalDef.FullRomData[11] = static_cast<char>(0x0A); // 10 -> x*0.5 -> 5
        ecuCalDef.NameList = {"Fuel"};
        ecuCalDef.MapData = {""};
        ecuCalDef.XScaleData = {""};
        ecuCalDef.YScaleData = {""};

        adapter.compute_map_cell_values(ecuCalDef, definition, "any_flash_method", 15);

        QCOMPARE(ecuCalDef.MapData.at(0), QString("5,"));
        QCOMPARE(ecuCalDef.XScaleData.at(0), QString(" "));
        QCOMPARE(ecuCalDef.YScaleData.at(0), QString(" "));
    }

    void compute_map_cell_values_applies_padding_before_decoding()
    {
        fastecu::definition::RomDefinition definition;
        fastecu::definition::Scaling scaling;
        scaling.name = "s";
        scaling.from_byte = "x";
        definition.scalings.push_back(scaling);
        fastecu::definition::CalibrationMap map;
        map.name = "Padded";
        map.address = 0x20000; // only valid after padding grows the ROM
        map.x_size = 1;
        map.y_size = 1;
        map.storage_type = "uint8";
        map.endian = "big";
        map.start_position = "1";
        map.interval = "1";
        map.scaling_name = "s";
        definition.maps.push_back(map);

        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray(160 * 1024, '\0');
        ecuCalDef.NameList = {"Padded"};
        ecuCalDef.MapData = {""};
        ecuCalDef.XScaleData = {""};
        ecuCalDef.YScaleData = {""};

        adapter.compute_map_cell_values(ecuCalDef, definition,
                                        "sub_ecu_denso_mc68hc16y5_02", 15);

        // Padding fills 0x20000..0x27FFF with 0xFF; the map at address 0x20000
        // reads a padding byte (0xFF == 255), proving padding ran first.
        QCOMPARE(ecuCalDef.MapData.at(0), QString("255,"));
    }

    void compute_map_cell_values_leaves_lists_unchanged_on_decode_failure()
    {
        fastecu::definition::RomDefinition definition;
        fastecu::definition::CalibrationMap map;
        map.name = "OutOfBounds";
        map.address = 999999; // far beyond the tiny ROM below
        map.x_size = 1;
        map.y_size = 1;
        map.storage_type = "uint8";
        map.start_position = "1";
        map.interval = "1";
        definition.maps.push_back(map);

        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray(10, '\0');
        ecuCalDef.NameList = {"OutOfBounds"};
        ecuCalDef.MapData = {"sentinel"};
        ecuCalDef.XScaleData = {"sentinel"};
        ecuCalDef.YScaleData = {"sentinel"};

        adapter.compute_map_cell_values(ecuCalDef, definition, "any_flash_method", 15);

        QCOMPARE(ecuCalDef.MapData.at(0), QString("sentinel"));
    }
};

QTEST_MAIN(TestLegacyCalibrationAdapter)
#include "test_legacy_calibration_adapter.moc"
