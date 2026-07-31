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

    void apply_flash_method_padding_grows_full_rom_data_in_place()
    {
        // Padding must persist into FullRomData, not just into a decode-only
        // copy: FileActions::open_subaru_rom_file validates the definition's
        // addresses against FullRomData's length, and every later consumer
        // (map edits, checksum correction, save-to-file, the flash writer)
        // addresses the same buffer.
        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray(160 * 1024, '\0');

        adapter.apply_flash_method_padding(ecuCalDef, "sub_ecu_denso_mc68hc16y5_02");

        QCOMPARE(ecuCalDef.FullRomData.length(), qsizetype{160} * 1024 + 0x8000);
        // 0x8000 bytes of 0xFF inserted at 0x20000, original bytes after it.
        QCOMPARE(static_cast<unsigned char>(ecuCalDef.FullRomData.at(0x20000)), 0xFFU);
        QCOMPARE(static_cast<unsigned char>(ecuCalDef.FullRomData.at(0x27FFF)), 0xFFU);
        QCOMPARE(static_cast<unsigned char>(ecuCalDef.FullRomData.at(0x28000)), 0x00U);
    }

    void apply_flash_method_padding_leaves_other_flash_methods_untouched()
    {
        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray(160 * 1024, '\0');

        adapter.apply_flash_method_padding(ecuCalDef, "any_flash_method");

        QCOMPARE(ecuCalDef.FullRomData.length(), qsizetype{160} * 1024);
    }

    void compute_map_cell_values_decodes_from_already_padded_rom_bytes()
    {
        // Named ..._rom_bytes, not ..._rom_data, deliberately: QtTest reads
        // a trailing "_data" as the data-driven data function for a
        // same-named test, so such a slot is silently never run as a test
        // of its own (observed, not theorized).
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

        // Padding is a separate step now (it has to persist before
        // validate_rom_size runs), so the caller applies it first and
        // compute_map_cell_values decodes the padded FullRomData as-is.
        adapter.apply_flash_method_padding(ecuCalDef, "sub_ecu_denso_mc68hc16y5_02");
        adapter.compute_map_cell_values(ecuCalDef, definition,
                                        "sub_ecu_denso_mc68hc16y5_02", 15);

        // Padding fills 0x20000..0x27FFF with 0xFF; the map at address 0x20000
        // reads a padding byte (0xFF == 255), so a decode against the
        // unpadded bytes could not have produced this.
        QCOMPARE(ecuCalDef.FullRomData.length(), qsizetype{160} * 1024 + 0x8000);
        QCOMPARE(ecuCalDef.MapData.at(0), QString("255,"));
    }

    void compute_map_cell_values_skips_only_the_failing_map()
    {
        // One map's decode failure leaves that map's three entries at
        // whatever they held (their " " placeholder in production) and
        // logs it -- the maps around it still get their decoded values.
        fastecu::definition::RomDefinition definition;
        fastecu::definition::Scaling scaling;
        scaling.name = "s";
        scaling.from_byte = "x";
        definition.scalings.push_back(scaling);

        fastecu::definition::CalibrationMap bad;
        bad.name = "OutOfBounds";
        bad.address = 999999; // far beyond the tiny ROM below
        bad.x_size = 1;
        bad.y_size = 1;
        bad.storage_type = "uint8";
        bad.start_position = "1";
        bad.interval = "1";
        bad.scaling_name = "s";
        definition.maps.push_back(bad);

        fastecu::definition::CalibrationMap good = bad;
        good.name = "InBounds";
        good.address = 4;
        definition.maps.push_back(good);

        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray(10, '\0');
        ecuCalDef.FullRomData[4] = static_cast<char>(0x07);
        ecuCalDef.NameList = {"OutOfBounds", "InBounds"};
        ecuCalDef.MapData = {"sentinel", "sentinel"};
        ecuCalDef.XScaleData = {"sentinel", "sentinel"};
        ecuCalDef.YScaleData = {"sentinel", "sentinel"};

        adapter.compute_map_cell_values(ecuCalDef, definition, "any_flash_method", 15);

        QCOMPARE(ecuCalDef.MapData.at(0), QString("sentinel"));
        QCOMPARE(ecuCalDef.XScaleData.at(0), QString("sentinel"));
        QCOMPARE(ecuCalDef.YScaleData.at(0), QString("sentinel"));
        QCOMPARE(ecuCalDef.MapData.at(1), QString("7,"));
    }

    void compute_map_cell_values_stops_at_the_shortest_of_the_three_lists()
    {
        // MapData/XScaleData/YScaleData are populated in lockstep, but the
        // write-back must not index past the end of the shorter two if they
        // ever diverge.
        fastecu::definition::RomDefinition definition;
        fastecu::definition::Scaling scaling;
        scaling.name = "s";
        scaling.from_byte = "x";
        definition.scalings.push_back(scaling);
        fastecu::definition::CalibrationMap map;
        map.name = "Fuel";
        map.address = 4;
        map.x_size = 1;
        map.y_size = 1;
        map.storage_type = "uint8";
        map.start_position = "1";
        map.interval = "1";
        map.scaling_name = "s";
        definition.maps.push_back(map);

        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray(10, '\0');
        ecuCalDef.FullRomData[4] = static_cast<char>(0x07);
        ecuCalDef.NameList = {"Fuel"};
        ecuCalDef.MapData = {""};
        ecuCalDef.XScaleData = {""};
        ecuCalDef.YScaleData = {}; // shorter than the other two

        adapter.compute_map_cell_values(ecuCalDef, definition, "any_flash_method", 15);

        QCOMPARE(ecuCalDef.YScaleData.size(), 0);
        QCOMPARE(ecuCalDef.MapData.at(0), QString("")); // skipped, not written
    }
};

QTEST_MAIN(TestLegacyCalibrationAdapter)
#include "test_legacy_calibration_adapter.moc"
