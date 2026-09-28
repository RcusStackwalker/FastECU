#include <QtTest>

#include "src/backend/calibration/legacy/legacy_calibration_adapter.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

using fastecu::InMemoryFileRepository;
using fastecu::calibration::LegacyCalibrationAdapter;
using fastecu::definitions::EcuCalDefStructure;

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
        fastecu::Status result = adapter.open_rom_bytes(ecuCalDef, "rom.bin", "");

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
        fastecu::Status result = adapter.open_rom_bytes(ecuCalDef, "", "cal/");

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
        fastecu::Status result = adapter.open_rom_bytes(ecuCalDef, "", "");

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, fastecu::ErrorKind::InvalidConfig);
    }

    void save_subaru_rom_file_writes_bytes_and_updates_names()
    {
        InMemoryFileRepository repo;
        LegacyCalibrationAdapter adapter(repo);
        EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray("\x01\x02", 2);

        EcuCalDefStructure *result = adapter.save_subaru_rom_file(&ecuCalDef, "saved/rom.bin");

        QVERIFY(result != nullptr);
        QCOMPARE(ecuCalDef.FullFileName, QString("saved/rom.bin"));
        QCOMPARE(ecuCalDef.FileName, QString("rom.bin"));
        QVERIFY(repo.files.count("saved/rom.bin"));
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
        QCOMPARE(static_cast<unsigned char>(ecu.FullRomData.at(0x20000)), 0xFFU);
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
        rom.scalings.push_back(fastecu::definition::Scaling{.name = "FuelScaling", .from_byte = "x"});
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
#include "legacy_calibration_adapter_test.moc"
