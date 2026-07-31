#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <clocale>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "test_file_actions_parsing.h"

namespace
{
class RecordingQtAtomicFileWriter : public fastecu::IAtomicFileWriter
{
  public:
    fastecu::Status replace(std::string_view handle,
                            std::span<const std::uint8_t> data) override
    {
        ++callCount;
        const fastecu::Status status = writer.replace(handle, data);
        if (status && afterSuccessfulReplace)
        {
            afterSuccessfulReplace();
        }
        return status;
    }

    int callCount{0};
    std::function<void()> afterSuccessfulReplace;

  private:
    QtAtomicFileWriter writer;
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

QString writeTextFileAt(const QString& path, const QByteArray& contents)
{
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

QString writeTextFile(const QTemporaryDir& dir,
                      const QString& name,
                      const QByteArray& contents)
{
    return writeTextFileAt(dir.filePath(name), contents);
}

// src/algorithms/expression/expression_evaluator.cpp parses numeric literals
// (e.g. the "0.5" in "x*0.5") via std::stod, which is sensitive to the
// process's current LC_NUMERIC locale: under a locale where ',' is the
// decimal separator (observed here with LANG=nl_NL.UTF-8, which QApplication
// picks up via its own setlocale(LC_ALL, "") call), std::stod("0.5") stops
// at the '.' and silently returns 0 instead of 0.5. That's a pre-existing,
// out-of-scope bug shared by both the legacy inline computation this task
// deletes and its replacement (both ultimately call the same
// expression_evaluate free function) -- not something Task 6 introduces or
// is chartered to fix. This guard makes the two tests below deterministic
// regardless of the host environment's locale.
class ScopedCNumericLocale
{
  public:
    ScopedCNumericLocale() : previous_(std::setlocale(LC_NUMERIC, nullptr) ? std::setlocale(LC_NUMERIC, nullptr) : "C")
    {
        std::setlocale(LC_NUMERIC, "C");
    }
    ~ScopedCNumericLocale()
    {
        std::setlocale(LC_NUMERIC, previous_.c_str());
    }
    ScopedCNumericLocale(const ScopedCNumericLocale&) = delete;
    ScopedCNumericLocale& operator=(const ScopedCNumericLocale&) = delete;

  private:
    std::string previous_;
};

bool spyContainsMessage(const QSignalSpy& spy, const QString& text)
{
    return std::ranges::any_of(spy, [&text](const QList<QVariant>& arguments)
                               { return !arguments.isEmpty() && arguments.at(0).toString().contains(text); });
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

    void malformed_romraider_catalog_file_is_skipped_and_replaces_with_empty_catalog()
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
        QSignalSpy errorSpy(&actions, &FileActions::LOG_E);

        QCOMPARE(actions.create_romraider_def_id_list(&config), &config);

        // A malformed file among a directory's worth of configured definitions is skipped
        // rather than treated as fatal (see DefinitionService::build_catalog), so this
        // replace succeeds with an empty catalog instead of preserving the old rows.
        QVERIFY(config.romraider_def_cal_id.isEmpty());
        QVERIFY(config.romraider_def_cal_id_addr.isEmpty());
        QVERIFY(config.romraider_def_ecu_id.isEmpty());
        QVERIFY(config.romraider_def_filename.isEmpty());
        QCOMPARE(errorSpy.count(), 0);
    }

    void malformed_romraider_definition_reports_definition_not_found()
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
        // The malformed file is skipped while building the catalog (see
        // DefinitionService::build_catalog), so "BROKEN" is simply absent from it rather than
        // failing with a parse error.
        QVERIFY(spyContainsMessage(errorSpy, "definition ID not found"));
        QVERIFY(spyContainsMessage(errorSpy, "BROKEN"));
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

        QVERIFY(atomicFileWriter_.replace_calls.empty());
        QVERIFY(actions.submittedEcuflashHandles_.empty());
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
        QVERIFY(atomicFileWriter_.replace_calls.empty());
        QVERIFY(actions.submittedEcuflashHandles_.empty());
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
        QCOMPARE(atomicFileWriter_.replace_calls.size(), std::size_t{1});
        auto parsed = fastecu::definition::parse_ecuflash_definition(
            atomicFileWriter_.replace_calls.front().data,
            atomicFileWriter_.replace_calls.front().handle);
        QVERIFY2(parsed.has_value(), parsed.error().detail.c_str());
        QCOMPARE(
            parsed->identity.internal_id_address,
            std::optional<std::uint64_t>{0x100});
    }

    void accepted_new_definition_records_xml_id_after_success_and_can_reload_external_destination()
    {
        QTemporaryDir configuredDirectory;
        QTemporaryDir destinationDirectory;
        QVERIFY(configuredDirectory.isValid());
        QVERIFY(destinationDirectory.isValid());
        const QString destination =
            destinationDirectory.filePath("created-external.xml");
        RecordingQtAtomicFileWriter writer;
        FileActions actions(
            fileSystem_, resourceBundle_, fileRepository_, writer);
        auto& config = actions.ConfigValuesStruct;
        config.ecuflash_definition_files_directory =
            configuredDirectory.path();
        QStringList idsDuringReplacement;
        QStringList sourcesDuringReplacement;
        writer.afterSuccessfulReplace = [&]()
        {
            idsDuringReplacement = config.ecuflash_def_cal_id;
            sourcesDuringReplacement = config.ecuflash_def_filename;
        };

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
                    editor->setText("  CREATE_XML  ");
                }
                else if (editor->objectName() == "internalidaddress")
                {
                    editor->setText("0");
                }
                else if (editor->objectName() == "internalidstring")
                {
                    editor->setText("CREATE_INTERNAL");
                }
                else if (editor->objectName() == "ecuid")
                {
                    editor->setText("CREATE_ECU");
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
        QCOMPARE(writer.callCount, 1);
        QVERIFY(idsDuringReplacement.isEmpty());
        QVERIFY(sourcesDuringReplacement.isEmpty());
        QCOMPARE(config.ecuflash_def_cal_id, QStringList({"CREATE_XML"}));
        QCOMPARE(config.ecuflash_def_cal_id_addr, QStringList({"0"}));
        QCOMPARE(config.ecuflash_def_ecu_id, QStringList({"CREATE_ECU"}));
        QCOMPARE(config.ecuflash_def_filename, QStringList({destination}));
        QCOMPARE(
            actions.definition_source(
                fastecu::definition::DefinitionFormat::EcuFlash,
                "CREATE_XML"),
            destination);

        ecu.FullRomData = QByteArray("CREATE_INTERNAL");
        QCOMPARE(
            actions.parse_ecuid_ecuflash_def_files(&ecu, true),
            &ecu);
        QCOMPARE(ecu.RomId, QString("CREATE_XML"));
        QCOMPARE(
            actions.read_ecuflash_ecu_def(&ecu, ecu.RomId),
            &ecu);
        QVERIFY(ecu.use_ecuflash_definition);
        QCOMPARE(
            ecu.RomInfo.at(FileActions::XmlId),
            QString("CREATE_XML"));
    }

    void accepted_imported_definition_records_xml_id_after_success_and_can_reload_external_destination()
    {
        QTemporaryDir configuredDirectory;
        QTemporaryDir sourceDirectory;
        QTemporaryDir destinationDirectory;
        QVERIFY(configuredDirectory.isValid());
        QVERIFY(sourceDirectory.isValid());
        QVERIFY(destinationDirectory.isValid());
        const QString source = writeTextFile(
            sourceDirectory,
            "source.xml",
            R"(<rom><romid><xmlid>OLD_XML</xmlid><internalidaddress>0</internalidaddress><internalidstring>OLD_INTERNAL</internalidstring></romid><vendor-extension/></rom>)");
        QVERIFY(!source.isEmpty());
        const QString destination =
            destinationDirectory.filePath("imported-external.xml");
        RecordingQtAtomicFileWriter writer;
        FileActions actions(
            fileSystem_, resourceBundle_, fileRepository_, writer);
        auto& config = actions.ConfigValuesStruct;
        config.ecuflash_definition_files_directory =
            configuredDirectory.path();
        QStringList idsDuringReplacement;
        QStringList sourcesDuringReplacement;
        writer.afterSuccessfulReplace = [&]()
        {
            idsDuringReplacement = config.ecuflash_def_cal_id;
            sourcesDuringReplacement = config.ecuflash_def_filename;
        };

        FileActions::EcuCalDefStructure ecu;
        bool handledSource = false;
        bool handledHeader = false;
        bool handledDestination = false;
        QTimer::singleShot(0, [&]()
                           {
            QFileDialog *sourcePicker = qobject_cast<QFileDialog *>(
                QApplication::activeModalWidget());
            if (!sourcePicker)
            {
                return;
            }
            sourcePicker->selectFile(source);
            handledSource = true;
            QTimer::singleShot(0, [&]()
                               {
                QDialog *dialog = qobject_cast<QDialog *>(
                    QApplication::activeModalWidget());
                if (!dialog)
                {
                    return;
                }
                for (QLineEdit *editor :
                     dialog->findChildren<QLineEdit *>())
                {
                    if (editor->objectName() == "xmlid")
                    {
                        editor->setText("  IMPORT_XML  ");
                    }
                    else if (editor->objectName() ==
                             "internalidaddress")
                    {
                        editor->setText("0");
                    }
                    else if (editor->objectName() ==
                             "internalidstring")
                    {
                        editor->setText("IMPORT_INTERNAL");
                    }
                    else if (editor->objectName() == "ecuid")
                    {
                        editor->setText("IMPORT_ECU");
                    }
                    else if (editor->objectName() == "include")
                    {
                        editor->clear();
                    }
                }
                handledHeader = true;
                QTimer::singleShot(0, [&]()
                                   {
                    QFileDialog *destinationPicker =
                        qobject_cast<QFileDialog *>(
                            QApplication::activeModalWidget());
                    if (!destinationPicker)
                    {
                        return;
                    }
                    destinationPicker->selectFile(destination);
                    handledDestination = true;
                    static_cast<QDialog *>(destinationPicker)->accept();
                });
                dialog->accept(); });
            static_cast<QDialog *>(sourcePicker)->accept(); });

        QCOMPARE(actions.use_existing_definition_for_rom(&ecu), &ecu);

        QVERIFY(handledSource);
        QVERIFY(handledHeader);
        QVERIFY(handledDestination);
        QCOMPARE(writer.callCount, 1);
        QVERIFY(idsDuringReplacement.isEmpty());
        QVERIFY(sourcesDuringReplacement.isEmpty());
        QCOMPARE(config.ecuflash_def_cal_id, QStringList({"IMPORT_XML"}));
        QCOMPARE(config.ecuflash_def_cal_id_addr, QStringList({"0"}));
        QCOMPARE(config.ecuflash_def_ecu_id, QStringList({"IMPORT_ECU"}));
        QCOMPARE(config.ecuflash_def_filename, QStringList({destination}));
        QCOMPARE(
            actions.definition_source(
                fastecu::definition::DefinitionFormat::EcuFlash,
                "IMPORT_XML"),
            destination);

        ecu.FullRomData = QByteArray("IMPORT_INTERNAL");
        QCOMPARE(
            actions.parse_ecuid_ecuflash_def_files(&ecu, true),
            &ecu);
        QCOMPARE(ecu.RomId, QString("IMPORT_XML"));
        QCOMPARE(
            actions.read_ecuflash_ecu_def(&ecu, ecu.RomId),
            &ecu);
        QVERIFY(ecu.use_ecuflash_definition);
        QCOMPARE(
            ecu.RomInfo.at(FileActions::XmlId),
            QString("IMPORT_XML"));
    }

    void directory_change_drops_discovered_sources_and_keeps_successful_submission()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const QString oldDirectory = workspace.filePath("old");
        const QString newDirectory = workspace.filePath("new");
        QVERIFY(QDir().mkpath(oldDirectory));
        QVERIFY(QDir().mkpath(newDirectory));
        const QString oldPath = writeTextFileAt(
            oldDirectory + "/old.xml",
            "<rom><romid><xmlid>OLD_DIRECTORY_XML</xmlid>"
            "<internalidaddress>10</internalidaddress>"
            "<internalidstring>OLD_DIRECTORY_INTERNAL</internalidstring>"
            "<ecuid>OLD_DIRECTORY_ECU</ecuid></romid></rom>");
        const QString newPath = writeTextFileAt(
            newDirectory + "/new.xml",
            "<rom><romid><xmlid>NEW_DIRECTORY_XML</xmlid>"
            "<internalidaddress>30</internalidaddress>"
            "<internalidstring>NEW_DIRECTORY_INTERNAL</internalidstring>"
            "<ecuid>NEW_DIRECTORY_ECU</ecuid></romid></rom>");
        const QString submittedPath =
            workspace.filePath("submitted.xml");
        QVERIFY(!oldPath.isEmpty());
        QVERIFY(!newPath.isEmpty());

        QtAtomicFileWriter writer;
        FileActions actions(
            fileSystem_, resourceBundle_, fileRepository_, writer);
        auto& config = actions.ConfigValuesStruct;
        config.ecuflash_definition_files_directory = oldDirectory;
        QCOMPARE(actions.create_ecuflash_def_id_list(&config), &config);
        QCOMPARE(
            config.ecuflash_def_cal_id,
            QStringList({"OLD_DIRECTORY_XML"}));

        auto input = validHeaderInput();
        input.xml_id = "SUBMITTED_XML";
        input.internal_id = "SUBMITTED_INTERNAL";
        input.ecu_id = "SUBMITTED_ECU";
        input.internal_id_address = 0x20;
        const fastecu::Status submitted =
            actions.submit_new_definition(
                submittedPath.toStdString(),
                input);
        QVERIFY2(
            submitted.has_value(),
            submitted.error().detail.c_str());
        QVERIFY(QFile::exists(submittedPath));

        config.ecuflash_definition_files_directory = newDirectory;
        QCOMPARE(actions.create_ecuflash_def_id_list(&config), &config);

        QCOMPARE(
            config.ecuflash_def_cal_id,
            QStringList({"NEW_DIRECTORY_XML", "SUBMITTED_XML"}));
        QCOMPARE(
            config.ecuflash_def_cal_id_addr,
            QStringList({"30", "20"}));
        QCOMPARE(
            config.ecuflash_def_ecu_id,
            QStringList({"NEW_DIRECTORY_ECU", "SUBMITTED_ECU"}));
        QCOMPARE(
            config.ecuflash_def_filename,
            QStringList({newPath, submittedPath}));
        QVERIFY(!config.ecuflash_def_filename.contains(oldPath));
    }

    void successful_submission_provenance_is_sorted_and_deduplicated()
    {
        atomicFileWriter_.reset();
        FileActions actions(
            fileSystem_,
            resourceBundle_,
            fileRepository_,
            atomicFileWriter_);
        const auto input = validHeaderInput();

        QVERIFY(actions.submit_new_definition("z.xml", input));
        QVERIFY(actions.submit_new_definition("a.xml", input));
        QVERIFY(actions.submit_new_definition("z.xml", input));

        QCOMPARE(
            actions.submittedEcuflashHandles_,
            (std::vector<std::string>{"a.xml", "z.xml"}));
    }

    void failed_definition_submission_logs_exact_error_and_preserves_catalog()
    {
        atomicFileWriter_.reset();
        const fastecu::Error backendError{
            fastecu::ErrorKind::Disconnected,
            "atomic destination unavailable",
        };
        atomicFileWriter_.replace_error = backendError;
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
        QVERIFY(actions.submittedEcuflashHandles_.empty());
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

    void open_subaru_rom_file_reads_bytes_and_sets_file_name()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString romPath = dir.filePath("rom.bin");
        QFile romFile(romPath);
        QVERIFY(romFile.open(QIODevice::WriteOnly));
        QCOMPARE(romFile.write(QByteArray("\xDE\xAD\xBE\xEF", 4)), qint64{4});
        romFile.close();

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);

        FileActions::EcuCalDefStructure *ecuCalDef = new FileActions::EcuCalDefStructure;
        while (ecuCalDef->RomInfo.length() < ecuCalDef->RomInfoStrings.length())
        {
            ecuCalDef->RomInfo.append(" ");
        }
        ecuCalDef = fileActions.open_subaru_rom_file(ecuCalDef, romPath);

        QVERIFY(ecuCalDef != nullptr);
        QCOMPARE(ecuCalDef->FullRomData, QByteArray("\xDE\xAD\xBE\xEF", 4));
        QCOMPARE(ecuCalDef->FileName, QString("rom.bin"));
        QCOMPARE(ecuCalDef->FullFileName, romPath);
        delete ecuCalDef;
    }

    void open_subaru_rom_file_returns_nullptr_on_read_failure()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        FileActions::EcuCalDefStructure *ecuCalDef = new FileActions::EcuCalDefStructure;
        while (ecuCalDef->RomInfo.length() < ecuCalDef->RomInfoStrings.length())
        {
            ecuCalDef->RomInfo.append(" ");
        }

        // The read failure still reports through QMessageBox::warning (that
        // dialog is FileActions's own, not the relocated chooser); dismiss
        // whatever modal appears rather than hang, matching this suite's
        // existing offscreen-QApplication convention.
        QTimer::singleShot(0, []()
                           {
            if (QWidget *modal = QApplication::activeModalWidget()) {
                modal->close();
} });

        FileActions::EcuCalDefStructure *result =
            fileActions.open_subaru_rom_file(ecuCalDef, dir.filePath("missing.bin"));

        QVERIFY(result == nullptr);
        delete ecuCalDef;
    }

    void open_subaru_rom_file_no_longer_persists_flash_method_padding()
    {
        // Historical note: this test previously pinned a final-review fix
        // (step5d-4 plan) that reordered open_subaru_rom_file's
        // size-validation block to run AFTER the (then still inline)
        // sub_ecu_denso_mc68hc16y5_02 padding block, so FullRomData grew by
        // 0x8000 bytes before the function returned. That inline padding
        // block was itself deleted by Task 6 of the step5d4b plan: padding
        // is now applied by LegacyCalibrationAdapter::compute_map_cell_values
        // (apply_flash_method_padding in legacy_calibration_adapter.cpp)
        // only to a local copy used for map-cell decoding, and is never
        // written back into ecuCalDef->FullRomData. So FullRomData's length
        // is now unaffected by the FlashMethod padding special case --
        // pinned here so a future change doesn't silently reintroduce
        // persistent padding of the caller's stored bytes.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString romPath = dir.filePath("rom.bin");
        QFile romFile(romPath);
        QVERIFY(romFile.open(QIODevice::WriteOnly));
        const QByteArray originalBytes(qsizetype{160} * 1024, '\0');
        QCOMPARE(romFile.write(originalBytes), qint64{originalBytes.size()});
        romFile.close();

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);

        FileActions::EcuCalDefStructure *ecuCalDef = new FileActions::EcuCalDefStructure;
        while (ecuCalDef->RomInfo.length() < ecuCalDef->RomInfoStrings.length())
        {
            ecuCalDef->RomInfo.append(" ");
        }
        ecuCalDef->RomInfo[FileActions::FlashMethod] = "sub_ecu_denso_mc68hc16y5_02";

        ecuCalDef = fileActions.open_subaru_rom_file(ecuCalDef, romPath);

        QVERIFY(ecuCalDef != nullptr);
        QCOMPARE(ecuCalDef->FullRomData.length(), originalBytes.length());
        delete ecuCalDef;
    }

    void save_subaru_rom_file_writes_bytes_via_calibration_adapter()
    {
        // FileActions::save_subaru_rom_file is now a one-line delegation to
        // LegacyCalibrationAdapter::save_subaru_rom_file (Task 6 of the
        // step5d-4 plan); this test exercises that delegation end-to-end
        // (the adapter itself already has its own dedicated coverage in
        // tests/test_legacy_calibration_adapter.cpp).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString romPath = dir.filePath("saved.bin");

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray("\xCA\xFE\xBA\xBE", 4);

        FileActions::EcuCalDefStructure *result =
            fileActions.save_subaru_rom_file(&ecuCalDef, romPath);

        QVERIFY(result == &ecuCalDef);
        QCOMPARE(ecuCalDef.FullFileName, romPath);
        QCOMPARE(ecuCalDef.FileName, QString("saved.bin"));

        QFile writtenFile(romPath);
        QVERIFY(writtenFile.open(QIODevice::ReadOnly));
        QCOMPARE(writtenFile.readAll(), QByteArray("\xCA\xFE\xBA\xBE", 4));
    }

    void open_subaru_rom_file_computes_map_data_for_a_matched_definition()
    {
        const ScopedCNumericLocale cLocale;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeTextFile(
            dir,
            "romraider.xml",
            R"(<roms>
  <rom>
    <romid>
      <xmlid>CAL_TEST</xmlid><internalidaddress>0</internalidaddress>
      <internalidstring>CAL_TEST</internalidstring>
    </romid>
    <table name="Fuel" type="2D" storagetype="uint16" endian="big"
           storageaddress="14" sizex="2" sizey="1">
      <scaling units="%" expression="x*0.5" to_byte="x*2"
               format="0.0" fineincrement="0.5" coarseincrement="1"/>
    </table>
  </rom>
</roms>)");
        // Deviation from the brief's literal snippet: storageaddress is
        // parsed as hex (parser_utils.cc's optional_hex_attribute, matching
        // RomRaider convention -- see also the legacy inline loop this task
        // deletes, which itself reads AddressList with base 16). The brief's
        // storageaddress="20" plus byte offset 20 would decode to hex 0x20 =
        // decimal 32, outside this 24-byte ROM, which trips
        // validate_rom_size's "address exceeds ROM size" QMessageBox::warning
        // and hangs the (non-interactive) test on QDialog::exec(). Using
        // "14" (hex 0x14 = decimal 20) keeps the intended decimal byte
        // offset and the brief's expected "5,10," result.
        QVERIFY(!definitionPath.isEmpty());

        const QString romPath = dir.filePath("cal_test.bin");
        QFile romFile(romPath);
        QVERIFY(romFile.open(QIODevice::WriteOnly));
        QByteArray romBytes(24, '\0');
        romBytes.replace(0, 8, "CAL_TEST"); // internal ID match at address 0
        romBytes[20] = static_cast<char>(0x00);
        romBytes[21] = static_cast<char>(0x0A); // 10 -> x*0.5 -> 5
        romBytes[22] = static_cast<char>(0x00);
        romBytes[23] = static_cast<char>(0x14); // 20 -> x*0.5 -> 10
        QCOMPARE(romFile.write(romBytes), qint64{romBytes.size()});
        romFile.close();

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = fileActions.ConfigValuesStruct;
        config.primary_definition_base = "romraider";
        config.use_romraider_definitions = "enabled";
        config.romraider_definition_files = {definitionPath};
        // read_romraider_ecu_def gates on ConfigValuesStruct.romraider_def_cal_id
        // already containing the matched cal id -- that list is populated by
        // create_romraider_def_id_list, which MainWindow calls once at startup
        // (mainwindow.cpp:189) before any open_subaru_rom_file call. Not part
        // of the brief's fixture snippet, but required for this path to reach
        // read_romraider_ecu_def at all; matches this suite's own
        // romraider_definition_indexes_ids_and_inherits_base convention.
        fileActions.create_romraider_def_id_list(&config);

        FileActions::EcuCalDefStructure *ecuCalDef = new FileActions::EcuCalDefStructure;
        while (ecuCalDef->RomInfo.length() < ecuCalDef->RomInfoStrings.length())
        {
            ecuCalDef->RomInfo.append(" ");
        }

        ecuCalDef = fileActions.open_subaru_rom_file(ecuCalDef, romPath);

        QVERIFY(ecuCalDef != nullptr);
        QVERIFY(ecuCalDef->use_romraider_definition);
        QCOMPARE(ecuCalDef->NameList.at(0), QString("Fuel"));
        QCOMPARE(ecuCalDef->MapData.at(0), QString("5,10,"));
        QCOMPARE(ecuCalDef->XScaleData.at(0), QString(" "));
        QCOMPARE(ecuCalDef->YScaleData.at(0), QString(" "));
        delete ecuCalDef;
    }

    void open_subaru_rom_file_writes_map_data_at_the_correct_index_for_multiple_maps()
    {
        const ScopedCNumericLocale cLocale;
        // Pins the design doc's Risk 2 (map ordering): ecuCalDef->NameList's
        // order (populated by read_romraider_ecu_def's dispatch) and
        // rom_definition.maps's order (populated by the separate
        // definitionService_.load call for validate_rom_size) are guaranteed
        // to match because both come from the same DefinitionService::load
        // call over the same catalog/format/id -- see legacy_definition_adapter.cpp:459-462.
        // Deliberately non-alphabetical map names (Zebra before Alpha) so a
        // positional mismatch would produce visibly wrong, not
        // coincidentally-right, results.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeTextFile(
            dir,
            "romraider-multi.xml",
            R"(<roms>
  <rom>
    <romid>
      <xmlid>MULTI_TEST</xmlid><internalidaddress>0</internalidaddress>
      <internalidstring>MULTI_TEST</internalidstring>
    </romid>
    <table name="Zebra" storagetype="uint16" endian="big"
           storageaddress="28" sizex="1" sizey="1">
      <scaling units="%" expression="x*1" to_byte="x" format="0.0"/>
    </table>
    <table name="Alpha" storagetype="uint16" endian="big"
           storageaddress="3C" sizex="1" sizey="1">
      <scaling units="%" expression="x*1" to_byte="x" format="0.0"/>
    </table>
  </rom>
</roms>)");
        // storageaddress values are hex (see the sibling single-map test's
        // comment above): "28"/"3C" = decimal 40/60, matching this test's
        // byte offsets below.
        QVERIFY(!definitionPath.isEmpty());

        const QString romPath = dir.filePath("multi_test.bin");
        QFile romFile(romPath);
        QVERIFY(romFile.open(QIODevice::WriteOnly));
        QByteArray romBytes(70, '\0');
        romBytes.replace(0, 10, "MULTI_TEST");
        romBytes[40] = static_cast<char>(0x00);
        romBytes[41] = static_cast<char>(0x2A); // Zebra -> 42
        romBytes[60] = static_cast<char>(0x00);
        romBytes[61] = static_cast<char>(0x63); // Alpha -> 99
        QCOMPARE(romFile.write(romBytes), qint64{romBytes.size()});
        romFile.close();

        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        auto& config = fileActions.ConfigValuesStruct;
        config.primary_definition_base = "romraider";
        config.use_romraider_definitions = "enabled";
        config.romraider_definition_files = {definitionPath};
        // See the sibling single-map test above for why this call is required.
        fileActions.create_romraider_def_id_list(&config);

        FileActions::EcuCalDefStructure *ecuCalDef = new FileActions::EcuCalDefStructure;
        while (ecuCalDef->RomInfo.length() < ecuCalDef->RomInfoStrings.length())
        {
            ecuCalDef->RomInfo.append(" ");
        }

        ecuCalDef = fileActions.open_subaru_rom_file(ecuCalDef, romPath);

        QVERIFY(ecuCalDef != nullptr);
        QVERIFY(ecuCalDef->NameList.size() >= 2);
        QCOMPARE(ecuCalDef->NameList.at(0), QString("Zebra"));
        QCOMPARE(ecuCalDef->NameList.at(1), QString("Alpha"));
        QCOMPARE(ecuCalDef->MapData.at(0), QString("42,"));
        QCOMPARE(ecuCalDef->MapData.at(1), QString("99,"));
        delete ecuCalDef;
    }

  private:
    // FileActions's constructor now takes the config/settings ports (Task
    // 11 of the step5d-1 plan); these are unused by the parsing paths this
    // test exercises, so plain default-constructed Qt port implementations
    // are sufficient.
    QtFileSystem fileSystem_;
    QtResourceBundle resourceBundle_;
    QtFileRepository fileRepository_;
    fastecu::InMemoryAtomicFileWriter atomicFileWriter_;
};

int run_test_file_actions_parsing(int argc, char **argv)
{
    QApplication app(argc, argv);
    TestFileActionsParsing test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_file_actions_parsing.moc"
