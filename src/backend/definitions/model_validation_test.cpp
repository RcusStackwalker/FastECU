#include <QtTest>

#include <type_traits>

#include "src/backend/definitions/ecu_cal_def.h"
#include "src/backend/definitions/file_actions.h"

static_assert(std::is_same_v<FileActions::EcuCalDefStructure, fastecu::definitions::EcuCalDefStructure>);

class TestModelValidation : public QObject
{
    Q_OBJECT

  private slots:
    void extracted_ecu_cal_def_keeps_legacy_defaults()
    {
        FileActions::EcuCalDefStructure value;
        QCOMPARE(value.RomInfoNames.at(FileActions::XmlId), QString("xmlid"));
        QCOMPARE(value.DefHeaderNames.last(), QString("notes"));
        QVERIFY(!value.OemEcuFile);
        QVERIFY(!value.SyncedWithEcu);
        QVERIFY(!value.use_romraider_definition);
        QVERIFY(!value.use_ecuflash_definition);

        const FileActions::EcuCalDefStructure same;
        QVERIFY(value == same);
        value.NameList.append("changed");
        QVERIFY(!(value == same));
    }

    void loggerValues_acceptMatchingRows()
    {
        FileActions::LogValuesStructure logValues;
        appendLoggerValue(logValues);

        QStringList errors;
        QVERIFY(FileActions::validate_logger_values(logValues, &errors));
        QVERIFY(errors.isEmpty());
    }

    void loggerValues_reportMissingRequiredId()
    {
        FileActions::LogValuesStructure logValues;
        appendLoggerValue(logValues);
        logValues.log_value_id[0] = "";

        QStringList errors;
        QVERIFY(!FileActions::validate_logger_values(logValues, &errors));
        QVERIFY(errors.contains("log_value.id[0] is required"));
    }

    void loggerSwitches_reportMismatchedRows()
    {
        FileActions::LogValuesStructure logValues;
        appendLoggerSwitch(logValues);
        logValues.log_switch_state.clear();

        QStringList errors;
        QVERIFY(!FileActions::validate_logger_switches(logValues, &errors));
        QVERIFY(errors.contains("log_switch.state has 0 entries, expected 1"));
    }

    void calibrationMaps_reportMismatchedRows()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        appendCalibrationMap(ecuCalDef);
        ecuCalDef.NameList.clear();

        QStringList errors;
        QVERIFY(!FileActions::validate_calibration_maps(ecuCalDef, &errors));
        QVERIFY(errors.contains("calibration_map.name has 0 entries, expected 1"));
    }

    void calibrationMaps_reportMissingSwapXYRow()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        appendCalibrationMap(ecuCalDef);
        ecuCalDef.SwapXYList.clear();

        QStringList errors;
        QVERIFY(!FileActions::validate_calibration_maps(ecuCalDef, &errors));
        QVERIFY(errors.contains("calibration_map.swap_xy has 0 entries, expected 1"));
    }

    void ecuflashBaseHeaderDefaultsMissingOptionalFields()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        const QStringList xmlLines = {
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n",
            "<rom>\n",
            "  <romid>\n",
            "    <xmlid>BASE_TEST</xmlid>\n",
            "    <internalidaddress>0x2000</internalidaddress>\n",
            "    <internalidstring>TESTID</internalidstring>\n",
            "    <ecuid>TESTECU</ecuid>\n",
            "    <make>Subaru</make>\n",
            "    <market>USDM</market>\n",
            "    <model>Impreza</model>\n",
            "    <year>2004</year>\n",
            "    <flashmethod>sub_ecu_denso_sh7055</flashmethod>\n",
            "    <memmodel>SH7055</memmodel>\n",
            "    <checksummodule>checksum_ecu_subaru_denso_sh7055</checksummodule>\n",
            "  </romid>\n",
            "</rom>\n",
        };

        int endIndex = -1;
        const QStringList headerData = FileActions::collect_ecuflash_base_header_fields(ecuCalDef, xmlLines, &endIndex);

        QCOMPARE(headerData.count("include"), 1);
        QCOMPARE(headerData.at(headerData.indexOf("include") + 1), QString());
        QCOMPARE(headerData.count("notes"), 1);
        QCOMPARE(headerData.at(headerData.indexOf("notes") + 1), QString());
        QVERIFY(endIndex > 0);
    }

    void ecuflashBaseBodyCopyKeepsMapsInsideGeneratedRom()
    {
        const QStringList xmlLines = {
            "<rom>\n",
            "  <romid>\n",
            "    <xmlid>4711a132</xmlid>\n",
            "  </romid>\n",
            "  <include>47110032</include>\n",
            "  <scaling name=\"Patch\" storagetype=\"bloblist\" />\n",
            "  <table name=\"Patch\" address=\"50022\" category=\"Patches\" type=\"1D\" scaling=\"Patch\" />\n",
            "</rom>\n",
        };

        const QStringList bodyLines = FileActions::collect_ecuflash_definition_body_lines(xmlLines, 5);

        QVERIFY(bodyLines.join("").contains("<table name=\"Patch\""));
        QVERIFY(!bodyLines.join("").contains("</rom>"));
    }

  private:
    static void appendLoggerValue(FileActions::LogValuesStructure& logValues)
    {
        logValues.log_value_protocol << "SSM";
        logValues.log_value_id << "P1";
        logValues.log_value_name << "Engine Speed";
        logValues.log_value_description << "RPM";
        logValues.log_value_ecu_byte_index << "0";
        logValues.log_value_ecu_bit << "0";
        logValues.log_value_target << "ECU";
        logValues.log_value_address << "0x1234";
        logValues.log_value_conversions.append(QList<fastecu::logging::Conversion>{{"rpm", "x", "0", "", "", ""}});
        logValues.log_value_length << "2";
        logValues.log_value << "0.00";
        logValues.log_value_enabled << "1";
    }

    static void appendLoggerSwitch(FileActions::LogValuesStructure& logValues)
    {
        logValues.log_switch_protocol << "SSM";
        logValues.log_switch_id << "S1";
        logValues.log_switch_name << "Switch";
        logValues.log_switch_description << "Description";
        logValues.log_switch_address << "0x20";
        logValues.log_switch_ecu_byte_index << "0";
        logValues.log_switch_ecu_bit << "1";
        logValues.log_switch_target << "ECU";
        logValues.log_switch_enabled << "0";
        logValues.log_switch_state << "0";
    }

    static void appendCalibrationMap(FileActions::EcuCalDefStructure& ecuCalDef)
    {
        ecuCalDef.IdList << "map1";
        ecuCalDef.TypeList << "3D";
        ecuCalDef.NameList << "Fuel";
        ecuCalDef.AddressList << "0x1000";
        ecuCalDef.CategoryList << "Fuel";
        ecuCalDef.XSizeList << "16";
        ecuCalDef.YSizeList << "16";
        ecuCalDef.FormatList << "0.00";
        ecuCalDef.UnitsList << "%";
        ecuCalDef.StorageTypeList << "uint16";
        ecuCalDef.EndianList << "big";
        ecuCalDef.FromByteList << "0";
        ecuCalDef.ToByteList << "1";
        ecuCalDef.MapDefined << "1";
        ecuCalDef.SubCategoryList << " ";
        ecuCalDef.LevelList << " ";
        ecuCalDef.UserLevelList << " ";
        ecuCalDef.SwapXYList << "false";
        ecuCalDef.FlipXList << "false";
        ecuCalDef.FlipYList << "false";
    }
};

QTEST_APPLESS_MAIN(TestModelValidation)

#include "model_validation_test.moc"
