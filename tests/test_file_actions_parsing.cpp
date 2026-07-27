#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/ports/atomic_file_writer.h"
#include "src/backend/definitions/file_actions.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "test_file_actions_parsing.h"

namespace
{
class InMemoryAtomicFileWriter : public fastecu::IAtomicFileWriter
{
  public:
    struct Call
    {
        std::string handle;
        std::vector<std::uint8_t> data;
    };

    fastecu::Status replace(std::string_view handle,
                            std::span<const std::uint8_t> data) override
    {
        calls.push_back(Call{
            .handle = std::string(handle),
            .data = std::vector<std::uint8_t>(data.begin(), data.end()),
        });
        if (error)
        {
            return std::unexpected(*error);
        }
        files[std::string(handle)] =
            std::vector<std::uint8_t>(data.begin(), data.end());
        return {};
    }

    void reset()
    {
        calls.clear();
        files.clear();
        error.reset();
    }

    std::vector<Call> calls;
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::optional<fastecu::Error> error;
};

fastecu::definition::DefinitionHeaderInput validHeaderInput()
{
    return fastecu::definition::DefinitionHeaderInput{
        .xml_id = "NEW_XML",
        .internal_id = "A1B2C3",
        .ecu_id = "ECU-42",
        .internal_id_address = 0x1A0,
        .metadata =
            fastecu::definition::RomMetadata{
                .make = "Subaru",
                .market = "EU",
                .model = "Legacy",
                .submodel = "GT",
                .transmission = "6MT",
                .year = "2008",
                .flash_method = "subaru_denso_can",
                .memory_model = "SH7058",
                .checksum_module = "subarudbw",
                .file_size = "1048576",
            },
        .include = "BASE_XML",
        .notes = "Document notes",
    };
}

QString writeTextFile(const QTemporaryDir& dir,
                      const QString& name,
                      const QByteArray& contents)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return {};
    }
    if (file.write(contents) != contents.size())
    {
        return {};
    }
    file.close();
    return path;
}

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

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
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

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
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

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
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

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
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

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
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

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        QSignalSpy debugSpy(&actions, &FileActions::LOG_D);
        actions.ConfigValuesStruct.romraider_definition_files = {definitionPath};
        QCOMPARE(actions.create_romraider_def_id_list(&actions.ConfigValuesStruct),
                 &actions.ConfigValuesStruct);

        const int idIndex = actions.ConfigValuesStruct.romraider_def_cal_id.indexOf("CAL_TEST");
        QVERIFY(idIndex >= 0);
        QCOMPARE(actions.ConfigValuesStruct.romraider_def_cal_id_addr.at(idIndex), QString("0"));
        QCOMPARE(actions.ConfigValuesStruct.romraider_def_filename.at(idIndex), definitionPath);

        FileActions::EcuCalDefStructure ecu;
        while (ecu.RomInfo.size() < ecu.RomInfoStrings.size())
        {
            ecu.RomInfo.append(" ");
        }
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
        QVERIFY(spyContainsMessage(debugSpy, "1 RomRaider definition files found"));
        QVERIFY(spyContainsMessage(debugSpy, "2 RomRaider ecu id's found"));
        QVERIFY(spyContainsMessage(debugSpy, "XML ID: CAL_TEST CAL_TEST"));
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

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
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
        {
            ecu.RomInfo.append(" ");
        }
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

    void malformed_romraider_catalog_preserves_existing_rows_and_logs_error()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath =
            writeTextFile(dir, "malformed-romraider.xml", "<roms><rom>");
        QVERIFY(!definitionPath.isEmpty());

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = actions.ConfigValuesStruct;
        config.romraider_definition_files = {definitionPath};
        config.romraider_def_cal_id = {"sentinel-id"};
        config.romraider_def_cal_id_addr = {"sentinel-address"};
        config.romraider_def_ecu_id = {"sentinel-ecu"};
        config.romraider_def_filename = {"sentinel-source"};
        const QStringList ids = config.romraider_def_cal_id;
        const QStringList addresses = config.romraider_def_cal_id_addr;
        const QStringList ecuIds = config.romraider_def_ecu_id;
        const QStringList sources = config.romraider_def_filename;
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);

        QCOMPARE(actions.create_romraider_def_id_list(&config), &config);

        QCOMPARE(config.romraider_def_cal_id, ids);
        QCOMPARE(config.romraider_def_cal_id_addr, addresses);
        QCOMPARE(config.romraider_def_ecu_id, ecuIds);
        QCOMPARE(config.romraider_def_filename, sources);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "malformed XML"));
    }

    void malformed_romraider_definition_preserves_caller_state()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath =
            writeTextFile(dir, "malformed-romraider.xml", "<roms><rom>");
        QVERIFY(!definitionPath.isEmpty());

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = actions.ConfigValuesStruct;
        config.romraider_definition_files = {definitionPath};
        config.romraider_def_cal_id = {"BROKEN"};
        config.romraider_def_cal_id_addr = {"0"};
        config.romraider_def_ecu_id = {"sentinel-ecu"};
        config.romraider_def_filename = {definitionPath};

        FileActions::EcuCalDefStructure ecu;
        ecu.RomInfo = QStringList(ecu.RomInfoStrings.size(), "sentinel-rom-info");
        ecu.DefinitionFileName = "sentinel-definition-file";
        ecu.NameList = {"sentinel-map"};
        ecu.use_romraider_definition = false;
        const QStringList romInfo = ecu.RomInfo;
        const QString definitionFileName = ecu.DefinitionFileName;
        const QStringList names = ecu.NameList;
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);

        QCOMPARE(actions.read_romraider_ecu_def(&ecu, "BROKEN"), &ecu);

        QCOMPARE(ecu.RomInfo, romInfo);
        QCOMPARE(ecu.DefinitionFileName, definitionFileName);
        QCOMPARE(ecu.NameList, names);
        QVERIFY(!ecu.use_romraider_definition);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "malformed XML"));
    }

    void romraider_base_missing_source_logs_context_and_preserves_state()
    {
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        FileActions::EcuCalDefStructure ecu;
        ecu.RomInfo =
            QStringList(ecu.RomInfoStrings.size(), "sentinel-rom-info");
        ecu.RomInfo[FileActions::XmlId] = "BASE";
        ecu.NameList = {"sentinel-map"};
        const FileActions::EcuCalDefStructure original = ecu;
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);

        QCOMPARE(actions.read_romraider_ecu_base_def(&ecu), nullptr);

        QVERIFY(ecu == original);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "RomRaider base definition"));
        QVERIFY(spyContainsMessage(errorSpy, "source"));
    }

    void romraider_base_missing_definition_id_logs_context_and_preserves_state()
    {
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        FileActions::EcuCalDefStructure ecu;
        ecu.DefinitionFileName = "base.xml";
        ecu.RomInfo =
            QStringList(ecu.RomInfoStrings.size(), " ");
        ecu.NameList = {"sentinel-map"};
        const FileActions::EcuCalDefStructure original = ecu;
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);

        QCOMPARE(actions.read_romraider_ecu_base_def(&ecu), nullptr);

        QVERIFY(ecu == original);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "RomRaider base definition"));
        QVERIFY(spyContainsMessage(errorSpy, "definition ID"));
    }

    void missing_romraider_base_returns_null_and_preserves_caller_state()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        FileActions::EcuCalDefStructure ecu;
        ecu.RomInfo = QStringList(ecu.RomInfoStrings.size(), "sentinel-rom-info");
        ecu.DefinitionFileName = dir.filePath("missing-romraider.xml");
        ecu.NameList = {"sentinel-map"};
        const QStringList romInfo = ecu.RomInfo;
        const QString definitionFileName = ecu.DefinitionFileName;
        const QStringList names = ecu.NameList;
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);
        QTimer::singleShot(0, []()
                           {
            if (QWidget *modal = QApplication::activeModalWidget())
            {
                modal->close();
            } });

        QCOMPARE(actions.read_romraider_ecu_base_def(&ecu), nullptr);

        QCOMPARE(ecu.RomInfo, romInfo);
        QCOMPARE(ecu.DefinitionFileName, definitionFileName);
        QCOMPARE(ecu.NameList, names);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "cannot open file"));
    }

    void romraider_rom_match_accepts_legacy_hex_identifier()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeTextFile(
            dir,
            "hex-romraider.xml",
            "<roms><rom><romid><xmlid>AB10</xmlid>"
            "<internalidaddress>0</internalidaddress>"
            "<internalidstring>AB10</internalidstring></romid></rom></roms>");
        QVERIFY(!definitionPath.isEmpty());

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = actions.ConfigValuesStruct;
        config.romraider_definition_files = {definitionPath};

        FileActions::EcuCalDefStructure ecu;
        ecu.FullRomData = QByteArray::fromHex("AB10");
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);

        QCOMPARE(
            actions.parse_ecuid_romraider_def_files(&ecu, false),
            &ecu);

        QCOMPARE(ecu.RomId, QString("AB10"));
        QVERIFY(errorSpy.isEmpty());
    }

    void malformed_compatibility_catalog_columns_preserve_rom_id()
    {
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = actions.ConfigValuesStruct;
        config.romraider_def_cal_id = {"AB10"};
        config.romraider_def_cal_id_addr = {"0", "1"};
        config.romraider_def_ecu_id = {};
        config.romraider_def_filename = {"hex-definition.xml"};

        FileActions::EcuCalDefStructure ecu;
        ecu.RomId = "sentinel-rom-id";
        ecu.FullRomData = QByteArray::fromHex("AB10");
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);

        QCOMPARE(
            actions.parse_ecuid_romraider_def_files(&ecu, false),
            &ecu);

        QCOMPARE(ecu.RomId, QString("sentinel-rom-id"));
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "ID/source/address/ECU"));
    }

    void cancelled_new_definition_dialog_never_submits()
    {
        atomicFileWriter_.reset();
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        FileActions::EcuCalDefStructure ecu;
        QTimer::singleShot(0, []()
                           {
            if (QDialog *dialog =
                    qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            {
                dialog->reject();
            } });

        QCOMPARE(actions.create_new_definition_for_rom(&ecu), &ecu);

        QVERIFY(atomicFileWriter_.calls.empty());
    }

    void cancelled_new_definition_destination_never_submits()
    {
        atomicFileWriter_.reset();
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        FileActions::EcuCalDefStructure ecu;
        bool handledHeader = false;
        bool handledDestination = false;
        bool handledRetry = false;
        QTimer::singleShot(0, [&]()
                           {
            QDialog *dialog =
                qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
            {
                return;
            }
            for (QLineEdit *editor : dialog->findChildren<QLineEdit *>())
            {
                if (editor->objectName() == "xmlid")
                {
                    editor->setText("NEW_XML");
                }
                else if (editor->objectName() == "internalidaddress")
                {
                    editor->setText("100");
                }
                else if (editor->objectName() == "internalidstring")
                {
                    editor->setText("A1B2C3");
                }
                else if (editor->objectName() == "ecuid")
                {
                    editor->setText("ECU-42");
                }
            }
            handledHeader = true;
            QTimer::singleShot(0, [&]()
                               {
                QFileDialog *picker = qobject_cast<QFileDialog *>(
                    QApplication::activeModalWidget());
                if (!picker)
                {
                    return;
                }
                handledDestination = true;
                QTimer::singleShot(0, [&]()
                                   {
                    QDialog *retry = qobject_cast<QDialog *>(
                        QApplication::activeModalWidget());
                    if (!retry)
                    {
                        return;
                    }
                    handledRetry = true;
                    retry->reject();
                });
                static_cast<QDialog *>(picker)->reject();
            });
            dialog->accept(); });

        QCOMPARE(actions.create_new_definition_for_rom(&ecu), &ecu);

        QVERIFY(handledHeader);
        QVERIFY(handledDestination);
        QVERIFY(handledRetry);
        QVERIFY(atomicFileWriter_.calls.empty());
    }

    void new_definition_dialog_treats_unprefixed_address_as_hexadecimal()
    {
        atomicFileWriter_.reset();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString destination = dir.filePath("created.xml");
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        FileActions::EcuCalDefStructure ecu;
        bool handledHeader = false;
        bool handledDestination = false;
        QTimer::singleShot(0, [&]()
                           {
            QDialog *dialog =
                qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
            {
                return;
            }
            for (QLineEdit *editor : dialog->findChildren<QLineEdit *>())
            {
                if (editor->objectName() == "xmlid")
                {
                    editor->setText("NEW_XML");
                }
                else if (editor->objectName() == "internalidaddress")
                {
                    editor->setText("100");
                }
                else if (editor->objectName() == "internalidstring")
                {
                    editor->setText("A1B2C3");
                }
                else if (editor->objectName() == "ecuid")
                {
                    editor->setText("ECU-42");
                }
            }
            handledHeader = true;
            QTimer::singleShot(0, [&]()
                               {
                QFileDialog *picker = qobject_cast<QFileDialog *>(
                    QApplication::activeModalWidget());
                if (!picker)
                {
                    return;
                }
                picker->selectFile(destination);
                handledDestination = true;
                static_cast<QDialog *>(picker)->accept();
            });
            dialog->accept(); });

        QCOMPARE(actions.create_new_definition_for_rom(&ecu), &ecu);

        QVERIFY(handledHeader);
        QVERIFY(handledDestination);
        QCOMPARE(atomicFileWriter_.calls.size(), std::size_t{1});
        auto parsed = fastecu::definition::parse_ecuflash_definition(
            atomicFileWriter_.calls.front().data,
            atomicFileWriter_.calls.front().handle);
        QVERIFY2(parsed.has_value(), parsed.error().detail.c_str());
        QCOMPARE(
            parsed->identity.internal_id_address,
            std::optional<std::uint64_t>{0x100});
    }

    void new_definition_submission_uses_one_atomic_replacement()
    {
        atomicFileWriter_.reset();
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        const auto input = validHeaderInput();

        const fastecu::Status result =
            actions.submit_new_definition("created.xml", input);

        QVERIFY2(result.has_value(), result.error().detail.c_str());
        QCOMPARE(atomicFileWriter_.calls.size(), std::size_t{1});
        QCOMPARE(atomicFileWriter_.calls.front().handle, std::string("created.xml"));
        auto parsed = fastecu::definition::parse_ecuflash_definition(
            atomicFileWriter_.calls.front().data,
            atomicFileWriter_.calls.front().handle);
        QVERIFY2(parsed.has_value(), parsed.error().detail.c_str());
        QCOMPARE(parsed->identity.xml_id, input.xml_id);
        QCOMPARE(parsed->identity.internal_id, input.internal_id);
        QCOMPARE(parsed->identity.ecu_id, input.ecu_id);
        QCOMPARE(parsed->identity.internal_id_address, input.internal_id_address);
        QCOMPARE(parsed->metadata, input.metadata);
        QCOMPARE(parsed->parents, std::vector<std::string>{input.include});
    }

    void imported_definition_submission_uses_one_atomic_replacement()
    {
        atomicFileWriter_.reset();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString source = writeTextFile(
            dir,
            "source.xml",
            R"(<rom><romid><xmlid>OLD</xmlid></romid><vendor-extension/></rom>)");
        QVERIFY(!source.isEmpty());
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        const auto input = validHeaderInput();

        const fastecu::Status result = actions.submit_imported_definition(
            source.toStdString(),
            "imported.xml",
            input);

        QVERIFY2(result.has_value(), result.error().detail.c_str());
        QCOMPARE(atomicFileWriter_.calls.size(), std::size_t{1});
        QCOMPARE(atomicFileWriter_.calls.front().handle, std::string("imported.xml"));
        const std::string xml(
            atomicFileWriter_.calls.front().data.begin(),
            atomicFileWriter_.calls.front().data.end());
        QVERIFY(xml.find("<vendor-extension") != std::string::npos);
    }

    void failed_definition_submission_logs_exact_error_and_preserves_catalog()
    {
        atomicFileWriter_.reset();
        const fastecu::Error backendError{
            fastecu::ErrorKind::Disconnected,
            "atomic destination unavailable",
        };
        atomicFileWriter_.error = backendError;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = actions.ConfigValuesStruct;
        config.ecuflash_def_cal_id = {"sentinel-id"};
        config.ecuflash_def_cal_id_addr = {"sentinel-address"};
        config.ecuflash_def_ecu_id = {"sentinel-ecu"};
        config.ecuflash_def_filename = {"sentinel-source"};
        const QStringList ids = config.ecuflash_def_cal_id;
        const QStringList addresses = config.ecuflash_def_cal_id_addr;
        const QStringList ecuIds = config.ecuflash_def_ecu_id;
        const QStringList sources = config.ecuflash_def_filename;
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);

        const fastecu::Status result =
            actions.submit_new_definition("unavailable.xml", validHeaderInput());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), backendError);
        QCOMPARE(config.ecuflash_def_cal_id, ids);
        QCOMPARE(config.ecuflash_def_cal_id_addr, addresses);
        QCOMPARE(config.ecuflash_def_ecu_id, ecuIds);
        QCOMPARE(config.ecuflash_def_filename, sources);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(spyContainsMessage(errorSpy, "Unable to create definition"));
        QVERIFY(spyContainsMessage(errorSpy, "Disconnected"));
        QVERIFY(spyContainsMessage(errorSpy, "atomic destination unavailable"));
    }

    void checksum_correction_unknown_mcu_type_logs_and_returns_unmodified_rom()
    {
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        actions.ConfigValuesStruct.flash_protocol_selected_make = "Subaru";
        actions.ConfigValuesStruct.flash_protocol_selected_checksum = "yes";
        actions.ConfigValuesStruct.flash_protocol_selected_protocol_name = "sub_ecu_hitachi_m32r_can";
        actions.ConfigValuesStruct.flash_protocol_selected_mcu = "M32170";

        FileActions::EcuCalDefStructure ecu;
        // "M32170" is sub_ecu_mitsu_m32r_can's real, currently shipped <mcu>
        // value in resources/shared/config/protocols.cfg; it has no
        // flashdevices[] entry, exercising checksum_correction's
        // unknown-MCU early return (no dialog, just a LOG_E signal).
        ecu.McuType = "M32170";
        ecu.RomId = "39670016";
        ecu.FullRomData = QByteArray(100, '\0');

        QCOMPARE(actions.checksum_correction(&ecu), &ecu);
        QCOMPARE(ecu.FullRomData, QByteArray(100, '\0'));
    }

    void checksum_correction_valid_mcu_corrects_rom_and_writes_back_bytes()
    {
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        actions.ConfigValuesStruct.flash_protocol_selected_make = "Subaru";
        actions.ConfigValuesStruct.flash_protocol_selected_checksum = "yes";
        actions.ConfigValuesStruct.flash_protocol_selected_protocol_name = "sub_ecu_denso_sh7055";
        actions.ConfigValuesStruct.flash_protocol_selected_mcu = "SH7055";

        FileActions::EcuCalDefStructure ecu;
        ecu.McuType = "SH7055";
        ecu.RomId = "39670016";
        ecu.FullRomData = QByteArray(524288, '\0'); // SH7055 romsize, all-zero -> Corrected
        ecu.use_romraider_definition = true;        // skip the "no definition linked" gate

        // The Corrected outcome shows a real "Checksums corrected"
        // QMessageBox; auto-dismiss whatever modal appears rather than
        // hang, matching this suite's existing offscreen-QApplication
        // convention (see _NEEDS_OFFSCREEN_QT_PLATFORM in
        // bazel/mut_dma_test_suites.bzl).
        QTimer::singleShot(0, []()
                           {
            if (QWidget *modal = QApplication::activeModalWidget()) {
                modal->close(); 
} });

        QCOMPARE(actions.checksum_correction(&ecu), &ecu);
        QCOMPARE(ecu.FullRomData.size(), 524288);
        QVERIFY(ecu.FullRomData != QByteArray(524288, '\0'));
    }

  private:
    // FileActions's constructor now takes the config/settings ports (Task
    // 11 of the step5d-1 plan); these are unused by the parsing paths this
    // test exercises, so plain default-constructed Qt port implementations
    // are sufficient.
    QtFileSystem fileSystem_;
    QtResourceBundle resourceBundle_;
    QtFileRepository fileRepository_;
    InMemoryAtomicFileWriter atomicFileWriter_;
};

int run_test_file_actions_parsing(int argc, char **argv)
{
    QApplication app(argc, argv);
    TestFileActionsParsing test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_file_actions_parsing.moc"
