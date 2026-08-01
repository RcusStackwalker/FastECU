#include <QtTest>

#include "src/backend/calibration/legacy_calibration_adapter.h"
#include "src/backend/definition/definition_model.h"
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
    //
    // The second <car_model> deliberately references a protocol name that is
    // absent from <protocols>. That is a real shape (see
    // car_model_catalog_test.cpp) and it is the case bind_protocol has to
    // fill with legacy's " " placeholder rather than leave stale. It is
    // appended last so the first car_model keeps index 0.
    return QByteArray(
        "<config>"
        "<protocols>"
        "<protocol name=\"sub_ecu_denso_can\" alias=\"denso_can\">"
        "<mcu>SH7058</mcu><checksum>yes</checksum><mode>can</mode>"
        "<log_protocol>ssm</log_protocol><description>Denso CAN</description>"
        "</protocol>"
        "</protocols>"
        "<car_models>"
        "<car_model>"
        "<make>Mitsubishi</make><model>Colt</model><version>Z27AG</version>"
        "<protocol>sub_ecu_denso_can</protocol>"
        "</car_model>"
        "<car_model>"
        "<make>Subaru</make><model>Impreza</model><version>GD</version>"
        "<protocol>orphan_protocol</protocol>"
        "</car_model>"
        "</car_models>"
        "</config>");
}

// Same shape, one <protocol> field changed, so a rebuild of the memoized
// join is observable.
QByteArray alternateXmlProtocolsFile()
{
    return QByteArray(
        "<config>"
        "<protocols>"
        "<protocol name=\"sub_ecu_denso_can\" alias=\"denso_can\">"
        "<mcu>SH72543</mcu><checksum>yes</checksum><mode>can</mode>"
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

void installProtocolsFile(fastecu::InMemoryFileRepository& repo, const char *handle,
                          const QByteArray& xml)
{
    repo.files[handle] = std::vector<std::uint8_t>(xml.begin(), xml.end());
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
        // The already-loaded path must not touch the repository's read side
        // at all -- it only backs the in-hand bytes up.
        QVERIFY(repo.read_handles.empty());
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
        installProtocolsFile(repo, "config/protocols.cfg", sampleXmlProtocolsFile());
        LegacyCalibrationAdapter adapter(repo);
        ConfigValuesStructure configValues;
        configValues.protocols_file = "config/protocols.cfg";

        adapter.bind_protocol(configValues, "sub_ecu_denso_can");

        QCOMPARE(configValues.flash_protocol_selected_id, QString("0"));
        QCOMPARE(configValues.flash_protocol_selected_make, QString("Mitsubishi"));
        QCOMPARE(configValues.flash_protocol_selected_model, QString("Colt"));
        QCOMPARE(configValues.flash_protocol_selected_version, QString("Z27AG"));
        QCOMPARE(configValues.flash_protocol_selected_protocol_name, QString("sub_ecu_denso_can"));
        QCOMPARE(configValues.flash_protocol_selected_description, QString("Denso CAN"));
        QCOMPARE(configValues.flash_protocol_selected_log_protocol, QString("ssm"));
        QCOMPARE(configValues.flash_protocol_selected_mcu, QString("SH7058"));
        QCOMPARE(configValues.flash_protocol_selected_checksum, QString("yes"));
    }

    void bind_protocol_leaves_selected_fields_untouched_on_no_match()
    {
        InMemoryFileRepository repo;
        installProtocolsFile(repo, "config/protocols.cfg", sampleXmlProtocolsFile());
        LegacyCalibrationAdapter adapter(repo);
        ConfigValuesStructure configValues;
        configValues.protocols_file = "config/protocols.cfg";
        configValues.flash_protocol_selected_mcu = "PRE_EXISTING";

        adapter.bind_protocol(configValues, "no_such_flash_method");

        QCOMPARE(configValues.flash_protocol_selected_mcu, QString("PRE_EXISTING"));
    }

    // A car model matches, but its protocol_name matches no <protocol>. The
    // legacy scan loop this replaced read the four protocol-derived scalars
    // out of the parallel flash_protocol_* lists, which
    // LegacyConfigAdapter::copy_car_models_into_legacy fills with a single
    // space for exactly this row shape -- so " ", not the matched row's
    // values and not whatever was there before.
    void bind_protocol_writes_the_legacy_placeholder_for_an_unmatched_protocol()
    {
        InMemoryFileRepository repo;
        installProtocolsFile(repo, "config/protocols.cfg", sampleXmlProtocolsFile());
        LegacyCalibrationAdapter adapter(repo);
        ConfigValuesStructure configValues;
        configValues.protocols_file = "config/protocols.cfg";

        adapter.bind_protocol(configValues, "orphan_protocol");

        QCOMPARE(configValues.flash_protocol_selected_id, QString("1"));
        QCOMPARE(configValues.flash_protocol_selected_make, QString("Subaru"));
        QCOMPARE(configValues.flash_protocol_selected_model, QString("Impreza"));
        QCOMPARE(configValues.flash_protocol_selected_version, QString("GD"));
        QCOMPARE(configValues.flash_protocol_selected_protocol_name, QString("orphan_protocol"));
        QCOMPARE(configValues.flash_protocol_selected_description, QString(" "));
        QCOMPARE(configValues.flash_protocol_selected_log_protocol, QString(" "));
        QCOMPARE(configValues.flash_protocol_selected_mcu, QString(" "));
        QCOMPARE(configValues.flash_protocol_selected_checksum, QString(" "));
    }

    // The regression this guards: bind_protocol used to write those four only
    // inside `if (row.protocol.has_value())`, so opening a ROM whose protocol
    // is unmatched kept the *previously* opened ROM's mcu/checksum -- and
    // file_actions.cpp branches on flash_protocol_selected_checksum to pick
    // the checksum module and assigns selected_mcu straight to
    // ecuCalDef->McuType.
    void bind_protocol_overwrites_a_stale_value_rather_than_leaving_it()
    {
        InMemoryFileRepository repo;
        installProtocolsFile(repo, "config/protocols.cfg", sampleXmlProtocolsFile());
        LegacyCalibrationAdapter adapter(repo);
        ConfigValuesStructure configValues;
        configValues.protocols_file = "config/protocols.cfg";

        adapter.bind_protocol(configValues, "sub_ecu_denso_can");
        QCOMPARE(configValues.flash_protocol_selected_mcu, QString("SH7058"));
        QCOMPARE(configValues.flash_protocol_selected_checksum, QString("yes"));

        adapter.bind_protocol(configValues, "orphan_protocol");

        QCOMPARE(configValues.flash_protocol_selected_mcu, QString(" "));
        QCOMPARE(configValues.flash_protocol_selected_checksum, QString(" "));
        QCOMPARE(configValues.flash_protocol_selected_description, QString(" "));
        QCOMPARE(configValues.flash_protocol_selected_log_protocol, QString(" "));
    }

    void bind_protocol_does_not_reread_protocols_file_on_a_second_call()
    {
        InMemoryFileRepository repo;
        installProtocolsFile(repo, "config/protocols.cfg", sampleXmlProtocolsFile());
        LegacyCalibrationAdapter adapter(repo);
        ConfigValuesStructure configValues;
        configValues.protocols_file = "config/protocols.cfg";

        adapter.bind_protocol(configValues, "sub_ecu_denso_can");
        const int readsAfterFirstCall = repo.read_count("config/protocols.cfg");
        QVERIFY(readsAfterFirstCall > 0);

        adapter.bind_protocol(configValues, "sub_ecu_denso_can");

        QCOMPARE(repo.read_count("config/protocols.cfg"), readsAfterFirstCall);
        QCOMPARE(configValues.flash_protocol_selected_mcu, QString("SH7058"));
    }

    void bind_protocol_rebuilds_the_cache_when_the_protocols_handle_changes()
    {
        InMemoryFileRepository repo;
        installProtocolsFile(repo, "config/protocols.cfg", sampleXmlProtocolsFile());
        installProtocolsFile(repo, "config/other.cfg", alternateXmlProtocolsFile());
        LegacyCalibrationAdapter adapter(repo);
        ConfigValuesStructure configValues;
        configValues.protocols_file = "config/protocols.cfg";

        adapter.bind_protocol(configValues, "sub_ecu_denso_can");
        QCOMPARE(configValues.flash_protocol_selected_mcu, QString("SH7058"));

        configValues.protocols_file = "config/other.cfg";
        adapter.bind_protocol(configValues, "sub_ecu_denso_can");

        QCOMPARE(configValues.flash_protocol_selected_mcu, QString("SH72543"));
        QVERIFY(repo.read_count("config/other.cfg") > 0);
    }

    // A first call that could not load the file must not poison the cache.
    void bind_protocol_retries_after_a_failed_load()
    {
        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        ConfigValuesStructure configValues;
        configValues.protocols_file = "config/protocols.cfg";

        adapter.bind_protocol(configValues, "sub_ecu_denso_can");
        QVERIFY(configValues.flash_protocol_selected_mcu.isEmpty());

        installProtocolsFile(repo, "config/protocols.cfg", sampleXmlProtocolsFile());
        adapter.bind_protocol(configValues, "sub_ecu_denso_can");

        QCOMPARE(configValues.flash_protocol_selected_mcu, QString("SH7058"));
    }

    void paddingPersistsIntoFullRomData()
    {
        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);

        EcuCalDefStructure ecu;
        // 0x2A000 (172,032 bytes), not the brief's original 0x30000: the
        // padding guard is "< 190*1024" (194,560 bytes), and 0x30000
        // (196,608 bytes) is already at/over that threshold, so it would
        // never trigger a pad. Task 5 hit and documented the same brief
        // inconsistency (task-5-report.md) and fixed it the same way.
        ecu.FullRomData = QByteArray(0x2A000, '\xAA');

        adapter.apply_flash_method_padding(ecu, "sub_ecu_denso_mc68hc16y5_02");

        // Must persist into FullRomData itself, not a copy: validate_rom_size and
        // every later consumer read this buffer.
        QCOMPARE(ecu.FullRomData.size(), 0x2A000 + 0x8000);
        QCOMPARE(static_cast<unsigned char>(ecu.FullRomData.at(0x20000)), 0xFFu);
    }

    void paddingLeavesOtherFlashMethodsAlone()
    {
        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);

        EcuCalDefStructure ecu;
        ecu.FullRomData = QByteArray(0x30000, '\xAA');

        adapter.apply_flash_method_padding(ecu, "sub_ecu_denso_sh7058");

        QCOMPARE(ecu.FullRomData.size(), 0x30000);
    }

    void computeMapCellValuesWritesMapAndAxisColumns()
    {
        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);

        fastecu::definition::RomDefinition rom;
        rom.scalings.push_back(fastecu::definition::Scaling{
            .name = "FuelScaling", .from_byte = "x"});
        fastecu::definition::CalibrationMap map;
        map.name = "Fuel";
        map.type = "2D";
        map.address = 0;
        map.x_size = 3;
        map.y_size = 1;
        map.storage_type = fastecu::definition::StorageType::Uint8;
        map.endian = "big";
        map.scaling_name = "FuelScaling";
        rom.maps.push_back(map);

        EcuCalDefStructure ecu;
        ecu.FullRomData = QByteArray::fromRawData("\x05\x06\x07", 3);
        // The legacy columns the adapter writes into are pre-sized by the
        // definition adapter; mirror that here.
        ecu.NameList = {"Fuel"};
        ecu.MapData = {" "};
        ecu.XScaleData = {" "};
        ecu.YScaleData = {" "};

        const fastecu::Status status = adapter.compute_map_cell_values(ecu, rom);
        QVERIFY(status.has_value());
        QCOMPARE(ecu.MapData.at(0), QString("5,6,7,"));
        QCOMPARE(ecu.XScaleData.at(0), QString(" "));
        QCOMPARE(ecu.YScaleData.at(0), QString(" "));
    }
};

QTEST_MAIN(TestLegacyCalibrationAdapter)
#include "test_legacy_calibration_adapter.moc"
