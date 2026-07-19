#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>

#include "src/backend/definitions/file_actions.h"
#include "test_rom_transformations.h"

namespace
{
QString writeBinaryFile(const QTemporaryDir& dir,
                        const QString& name,
                        const QByteArray& contents)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    if (file.write(contents) != contents.size())
        return {};
    file.close();
    return path;
}
} // namespace

class TestRomTransformations : public QObject
{
    Q_OBJECT

  private slots:
    void definition_backed_rom_decodes_storage_types_scaling_and_axes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString definitionPath = writeBinaryFile(
            dir,
            "romraider.xml",
            R"(<roms>
  <rom>
    <romid><xmlid>BASE_TRANSFORM</xmlid></romid>
    <table name="U8Scaled" type="1D" storagetype="uint8" endian="big">
      <scaling units="raw" expression="x*0.5" to_byte="x*2" format="0.0" fineincrement="0.5" coarseincrement="1"/>
    </table>
    <table name="U16Big" type="1D" storagetype="uint16" endian="big">
      <scaling units="raw" expression="x" to_byte="x" format="0" fineincrement="1" coarseincrement="10"/>
    </table>
    <table name="U24LittleCompatibility" type="1D" storagetype="uint24" endian="little">
      <scaling units="raw" expression="x" to_byte="x" format="0" fineincrement="1" coarseincrement="10"/>
    </table>
    <table name="U32Big" type="1D" storagetype="uint32" endian="big">
      <scaling units="raw" expression="x" to_byte="x" format="0" fineincrement="1" coarseincrement="10"/>
    </table>
    <table name="FloatCompatibility" type="1D" storagetype="float" endian="big">
      <scaling units="raw" expression="x" to_byte="x" format="0.0" fineincrement="0.1" coarseincrement="1"/>
    </table>
    <table name="Grid" type="3D" storagetype="uint8" endian="big">
      <scaling units="raw" expression="x" to_byte="x" format="0" fineincrement="1" coarseincrement="10"/>
      <table name="X" type="X Axis" storagetype="uint8" endian="big">
        <scaling units="x" expression="x" to_byte="x" format="0" fineincrement="1" coarseincrement="10"/>
      </table>
      <table name="Y" type="Y Axis" storagetype="uint8" endian="big">
        <scaling units="y" expression="x" to_byte="x" format="0" fineincrement="1" coarseincrement="10"/>
      </table>
    </table>
  </rom>
  <rom base="BASE_TRANSFORM">
    <romid>
      <xmlid>CAL1</xmlid><internalidaddress>0</internalidaddress>
      <internalidstring>CAL1</internalidstring><ecuid>TEST_ECU</ecuid>
      <make>Test</make><model>Synthetic</model><memmodel>M32R_128KB</memmodel>
      <flashmethod>test_protocol</flashmethod><filesize>128</filesize>
    </romid>
    <table name="U8Scaled" storageaddress="20" sizex="1" sizey="1"/>
    <table name="U16Big" storageaddress="22" sizex="1" sizey="1"/>
    <table name="U24LittleCompatibility" storageaddress="24" sizex="1" sizey="1"/>
    <table name="U32Big" storageaddress="28" sizex="1" sizey="1"/>
    <table name="FloatCompatibility" storageaddress="2c" sizex="1" sizey="1"/>
    <table name="Grid" storageaddress="40" sizex="2" sizey="2">
      <table name="X" type="X Axis" storageaddress="50"/>
      <table name="Y" type="Y Axis" storageaddress="60"/>
    </table>
  </rom>
</roms>)");
        QVERIFY(!definitionPath.isEmpty());

        QByteArray rom(128, static_cast<char>(0xAA));
        rom.replace(0, 4, QByteArray("CAL1", 4));
        rom.replace(0x20, 1, QByteArray::fromHex("08"));
        rom.replace(0x22, 2, QByteArray::fromHex("1234"));
        rom.replace(0x24, 3, QByteArray::fromHex("123456"));
        rom.replace(0x28, 4, QByteArray::fromHex("12345678"));
        rom.replace(0x2c, 4, QByteArray::fromHex("3f800000"));
        rom.replace(0x40, 4, QByteArray::fromHex("01020304"));
        rom.replace(0x50, 2, QByteArray::fromHex("0a14"));
        rom.replace(0x60, 2, QByteArray::fromHex("1e28"));
        const QByteArray original = rom;

        const QString romPath = writeBinaryFile(dir, "synthetic.bin", rom);
        QVERIFY(!romPath.isEmpty());

        FileActions actions;
        actions.ConfigValuesStruct.primary_definition_base = "romraider";
        actions.ConfigValuesStruct.use_romraider_definitions = "enabled";
        actions.ConfigValuesStruct.use_ecuflash_definitions = "disabled";
        actions.ConfigValuesStruct.romraider_definition_files = {definitionPath};
        actions.ConfigValuesStruct.romraider_def_cal_id = {"CAL1"};
        actions.ConfigValuesStruct.romraider_def_cal_id_addr = {"0"};
        actions.ConfigValuesStruct.romraider_def_ecu_id = {"TEST_ECU"};
        actions.ConfigValuesStruct.romraider_def_filename = {definitionPath};

        FileActions::EcuCalDefStructure ecu;
        QCOMPARE(actions.open_subaru_rom_file(&ecu, romPath), &ecu);

        QCOMPARE(ecu.NameList,
                 QStringList({"U8Scaled",
                              "U16Big",
                              "U24LittleCompatibility",
                              "U32Big",
                              "FloatCompatibility",
                              "Grid"}));
        QCOMPARE(ecu.MapData.at(0), QString("4,"));
        QCOMPARE(ecu.MapData.at(1), QString("4660,"));
        // Compatibility: open_subaru_rom_file fills mapDataValue but evaluates
        // zero dataByte for non-float little-endian integers.
        QCOMPARE(ecu.MapData.at(2), QString("0,"));
        QCOMPARE(ecu.MapData.at(3), QString("305419896,"));
        QCOMPARE(ecu.MapData.at(4), QString("1,"));
        QCOMPARE(ecu.MapData.at(5), QString("1,2,3,4,"));
        QCOMPARE(ecu.XScaleAddressList.at(5), QString("50"));
        QCOMPARE(ecu.YScaleAddressList.at(5), QString("60"));
        QCOMPARE(ecu.XScaleData.at(5), QString("10,20,"));
        QCOMPARE(ecu.YScaleData.at(5), QString("30,40,"));
        QCOMPARE(ecu.FullRomData, original);
    }
};

int run_test_rom_transformations(int argc, char **argv)
{
    QApplication app(argc, argv);
    TestRomTransformations test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_rom_transformations.moc"
