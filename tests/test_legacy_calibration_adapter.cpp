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
        repo.files["config/protocols.cfg"] =
            std::vector<std::uint8_t>(sampleXmlProtocolsFile().begin(), sampleXmlProtocolsFile().end());
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
        repo.files["config/protocols.cfg"] =
            std::vector<std::uint8_t>(sampleXmlProtocolsFile().begin(), sampleXmlProtocolsFile().end());
        LegacyCalibrationAdapter adapter(repo);
        ConfigValuesStructure configValues;
        configValues.protocols_file = "config/protocols.cfg";
        configValues.flash_protocol_selected_mcu = "PRE_EXISTING";

        adapter.bind_protocol(configValues, "no_such_flash_method");

        QCOMPARE(configValues.flash_protocol_selected_mcu, QString("PRE_EXISTING"));
    }
};

QTEST_MAIN(TestLegacyCalibrationAdapter)
#include "test_legacy_calibration_adapter.moc"
