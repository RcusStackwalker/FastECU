#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>

#include "file_actions.h"
#include "test_file_actions_parsing.h"

namespace
{
QString writeTextFile(const QTemporaryDir& dir,
                      const QString& name,
                      const QByteArray& contents)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    if (file.write(contents) != contents.size())
        return {};
    file.close();
    return path;
}
} // namespace

class TestFileActionsParsing : public QObject
{
    Q_OBJECT

  private slots:
    void application_config_reads_valid_values_and_preserves_defaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTextFile(
            dir,
            "fastecu.cfg",
            R"(<config name="FastECU" version="test">
  <software_settings>
    <setting name="window_size"><value width="1024"/><value height="768"/></setting>
    <setting name="toolbar_iconsize"><value data="24"/></setting>
    <setting name="serial_port"><value data="TEST_PORT"/></setting>
    <setting name="protocol_id"><value data="7"/></setting>
    <setting name="flash_transport"><value data="iso15765"/></setting>
    <setting name="log_transport"><value data="K-Line"/></setting>
    <setting name="log_protocol"><value data="SSM"/></setting>
  </software_settings>
</config>)");
        QVERIFY(!path.isEmpty());

        FileActions actions;
        FileActions::ConfigValuesStructure config;
        config.config_file = path;

        QCOMPARE(actions.read_config_file(&config), &config);
        QCOMPARE(config.window_width, QString("1024"));
        QCOMPARE(config.window_height, QString("768"));
        QCOMPARE(config.toolbar_iconsize, QString("24"));
        QCOMPARE(config.serial_port, QString("TEST_PORT"));
        QCOMPARE(config.flash_protocol_selected_id, QString("7"));
        QCOMPARE(config.flash_protocol_selected_flash_transport, QString("iso15765"));
        QCOMPARE(config.flash_protocol_selected_log_transport, QString("K-Line"));
        QCOMPARE(config.flash_protocol_selected_log_protocol, QString("SSM"));
        QCOMPARE(config.baudrate, QString("4800"));
        QCOMPARE(config.window_size, QString("default"));
        QCOMPARE(config.use_romraider_definitions, QString("disabled"));
        QCOMPARE(config.use_ecuflash_definitions, QString("disabled"));
        QCOMPARE(config.primary_definition_base, QString("ecuflash"));
    }

    void application_config_malformed_retains_defaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTextFile(dir, "malformed.cfg", "<config><software_settings>");
        QVERIFY(!path.isEmpty());

        FileActions actions;
        FileActions::ConfigValuesStructure config;
        config.config_file = path;

        QCOMPARE(actions.read_config_file(&config), &config);
        QCOMPARE(config.serial_port, QString("ttyUSB0"));
        QCOMPARE(config.toolbar_iconsize, QString("32"));
        QCOMPARE(config.use_romraider_definitions, QString("disabled"));
    }

    void logger_definition_reads_parameter_and_switch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTextFile(
            dir,
            "logger.xml",
            R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1" name="Engine Speed" desc="RPM" length="2">
    <address>0x1234</address>
    <conversions><conversion units="rpm" expr="x*0.25" format="0.00"
      gauge_min="0" gauge_max="8000" gauge_step="500"/></conversions>
  </parameter>
</parameters><switches>
  <switch id="S1" name="Test Switch" desc="flag" byte="0x20" bit="1" target="ECU"/>
</switches></protocol></protocols></logger>)");
        QVERIFY(!path.isEmpty());

        FileActions actions;
        actions.ConfigValuesStruct.romraider_logger_definition_file = path;

        FileActions::LogValuesStructure *values = actions.read_logger_definition_file();
        QCOMPARE(values, &actions.LogValuesStruct);
        QCOMPARE(values->log_value_protocol.at(0), QString("SSM"));
        QCOMPARE(values->log_value_id.at(0), QString("P1"));
        QCOMPARE(values->log_value_name.at(0), QString("Engine Speed"));
        QCOMPARE(values->log_value_address.at(0), QString("0x1234"));
        QCOMPARE(values->log_value_length.at(0), QString("2"));
        QCOMPARE(values->log_value_units.at(0),
                 QString("conversion 0,rpm,x*0.25,0.00,0,8000,500"));
        QCOMPARE(values->log_switch_id.at(0), QString("S1"));
        QCOMPARE(values->log_switch_address.at(0), QString("0x20"));
        QCOMPARE(values->log_switch_ecu_bit.at(0), QString("1"));
    }

    void logger_definition_uses_optional_attribute_defaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTextFile(
            dir,
            "logger-defaults.xml",
            R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1" name="Engine Speed" desc="RPM" length="2">
    <address>0x1234</address>
    <conversions><conversion units="rpm" expr="x*0.25" format="0.00"
      gauge_min="0" gauge_max="8000" gauge_step="500"/></conversions>
  </parameter>
  <parameter id="P2" name="Defaulted">
    <address>0x5678</address>
    <conversions><conversion/></conversions>
  </parameter>
</parameters></protocol></protocols></logger>)");
        QVERIFY(!path.isEmpty());

        FileActions actions;
        actions.ConfigValuesStruct.romraider_logger_definition_file = path;

        FileActions::LogValuesStructure *values = actions.read_logger_definition_file();
        QCOMPARE(values, &actions.LogValuesStruct);
        QCOMPARE(values->log_value_id.at(1), QString("P2"));
        QCOMPARE(values->log_value_description.at(1), QString("No desc"));
        QCOMPARE(values->log_value_length.at(1), QString("1"));
        QCOMPARE(values->log_value_ecu_byte_index.at(1), QString("No byte index"));
        QCOMPARE(values->log_value_target.at(1), QString("No target"));
    }

    void logger_config_reads_selected_ids()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTextFile(
            dir,
            "logger.cfg",
            R"(<config><logger><ecu id="TEST_ECU"><protocol id="SSM"><parameters>
  <gauges><parameter id="P1" name=""/></gauges>
  <lower_panel><parameter id="P2" name=""/></lower_panel>
</parameters><switches><switch id="S1" name=""/></switches>
</protocol></ecu></logger></config>)");
        QVERIFY(!path.isEmpty());

        FileActions actions;
        actions.ConfigValuesStruct.logger_file = path;
        FileActions::LogValuesStructure values;

        QCOMPARE(actions.read_logger_conf(&values, "TEST_ECU", false), &values);
        QCOMPARE(values.logging_values_protocol, QString("SSM"));
        QCOMPARE(values.dashboard_log_value_id, QStringList({"P1"}));
        QCOMPARE(values.lower_panel_log_value_id, QStringList({"P2"}));
        QCOMPARE(values.lower_panel_switch_id, QStringList({"S1"}));
    }

    void romraider_definition_indexes_ids_and_inherits_base()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeTextFile(
            dir,
            "romraider.xml",
            R"(<roms>
  <rom>
    <romid><xmlid>BASE_TEST</xmlid></romid>
    <table name="Fuel" type="2D" storagetype="uint16" endian="big">
      <scaling units="%" expression="x*0.5" to_byte="x*2"
               format="0.0" fineincrement="0.5" coarseincrement="1"/>
    </table>
  </rom>
  <rom base="BASE_TEST">
    <romid>
      <xmlid>CAL_TEST</xmlid><internalidaddress>0</internalidaddress>
      <internalidstring>CAL_TEST</internalidstring><ecuid>TEST_ECU</ecuid>
    </romid>
    <table name="Fuel" storageaddress="20" sizex="2" sizey="1"/>
  </rom>
</roms>)");
        QVERIFY(!definitionPath.isEmpty());

        FileActions actions;
        actions.ConfigValuesStruct.romraider_definition_files = {definitionPath};
        QCOMPARE(actions.create_romraider_def_id_list(&actions.ConfigValuesStruct),
                 &actions.ConfigValuesStruct);

        const int idIndex = actions.ConfigValuesStruct.romraider_def_cal_id.indexOf("CAL_TEST");
        QVERIFY(idIndex >= 0);
        QCOMPARE(actions.ConfigValuesStruct.romraider_def_cal_id_addr.at(idIndex), QString("0"));
        QCOMPARE(actions.ConfigValuesStruct.romraider_def_filename.at(idIndex), definitionPath);

        FileActions::EcuCalDefStructure ecu;
        while (ecu.RomInfo.size() < ecu.RomInfoStrings.size())
            ecu.RomInfo.append(" ");
        QCOMPARE(actions.read_romraider_ecu_def(&ecu, "CAL_TEST"), &ecu);

        QCOMPARE(ecu.RomInfo.at(FileActions::XmlId), QString("CAL_TEST"));
        QCOMPARE(ecu.NameList.at(0), QString("Fuel"));
        QCOMPARE(ecu.AddressList.at(0), QString("20"));
        QCOMPARE(ecu.XSizeList.at(0), QString("2"));
        QCOMPARE(ecu.YSizeList.at(0), QString("1"));
        QCOMPARE(ecu.StorageTypeList.at(0), QString("uint16"));
        QCOMPARE(ecu.EndianList.at(0), QString("big"));
        QCOMPARE(ecu.FromByteList.at(0), QString("x*0.5"));
        QCOMPARE(ecu.FormatList.at(0), QString("0.0"));
    }

    void romraider_definition_uses_blank_optional_rom_id_fields()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeTextFile(
            dir,
            "romraider-minimal.xml",
            "<roms><rom><romid><xmlid>MINIMAL_TEST</xmlid></romid></rom></roms>");
        QVERIFY(!definitionPath.isEmpty());

        FileActions actions;
        actions.ConfigValuesStruct.romraider_definition_files = {definitionPath};
        QCOMPARE(actions.create_romraider_def_id_list(&actions.ConfigValuesStruct),
                 &actions.ConfigValuesStruct);

        const int idIndex = actions.ConfigValuesStruct.romraider_def_cal_id.indexOf("MINIMAL_TEST");
        QVERIFY(idIndex >= 0);
        QCOMPARE(actions.ConfigValuesStruct.romraider_def_cal_id_addr.at(idIndex), QString(""));
        QCOMPARE(actions.ConfigValuesStruct.romraider_def_ecu_id.at(idIndex), QString(""));
        QCOMPARE(actions.ConfigValuesStruct.romraider_def_filename.at(idIndex), definitionPath);

        FileActions::EcuCalDefStructure ecu;
        while (ecu.RomInfo.size() < ecu.RomInfoStrings.size())
            ecu.RomInfo.append(" ");
        QCOMPARE(actions.read_romraider_ecu_def(&ecu, "MINIMAL_TEST"), &ecu);

        QCOMPARE(ecu.RomInfo.at(FileActions::XmlId), QString("MINIMAL_TEST"));
        QCOMPARE(ecu.RomInfo.at(FileActions::InternalIdAddress), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::InternalIdString), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::EcuId), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::Make), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::Market), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::Model), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::SubModel), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::Transmission), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::Year), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::FlashMethod), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::MemModel), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::ChecksumModule), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::FileSize), QString(""));
        QCOMPARE(ecu.RomInfo.at(FileActions::DefFile), definitionPath);
    }
};

int run_test_file_actions_parsing(int argc, char **argv)
{
    QApplication app(argc, argv);
    TestFileActionsParsing test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_file_actions_parsing.moc"
