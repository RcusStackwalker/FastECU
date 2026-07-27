#pragma once

#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QWidget>
#include <QFileDialog>
#include <QDomDocument>
#include <QXmlStreamReader>
#include <QMessageBox>
#include <QDebug>
#include <QSignalMapper>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDirIterator>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QTextEdit>

#include <cstdint>
#include <string.h>
#include <iostream>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/definitions/config_values.h"
#include "src/backend/checksum/legacy_checksum_adapter.h"
#include "src/backend/config/legacy_config_adapter.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/resource_bundle.h"

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif // Windows

class FileActions : public QWidget
{
    Q_OBJECT

  public:
    FileActions(fastecu::IFileSystem& file_system, fastecu::IResourceBundle& resource_bundle,
                fastecu::IFileRepository& file_repository, QWidget *parent = nullptr);

    uint8_t float_precision = 15;
    int def_map_index = 0;
    // QString ecu_protocol;

    // Defined in config_values.h (see that file's comment for why it is not
    // a nested struct here anymore) and re-exposed under its historical
    // name so every existing `FileActions::ConfigValuesStructure` call site
    // keeps compiling unchanged.
    using ConfigValuesStructure = fastecu::definitions::ConfigValuesStructure;
    ConfigValuesStructure ConfigValuesStruct;

    struct protocolsStructure
    {
        QStringList protocols;
        QStringList baudrate;
        QStringList databits;
        QStringList stopbits;
        QStringList parity;
        QStringList connect_timeout;
        QStringList send_timeout;
    } protocolsStruct;

    struct LogValuesStructure
    {
        QString ecu_id;
        QStringList log_value_protocol;
        QStringList log_value_id;
        QStringList log_value_name;
        QStringList log_value_description;
        QStringList log_value_ecu_byte_index;
        QStringList log_value_ecu_bit;
        QStringList log_value_target;
        QStringList log_value_address;
        QStringList log_value_units;
        QStringList log_value_from_byte;
        QStringList log_value_format;
        QStringList log_value_gauge_min;
        QStringList log_value_gauge_max;
        QStringList log_value_gauge_step;

        QStringList log_value_ecu_id;
        QStringList log_value_length;
        QStringList log_value_type;
        QStringList log_value;

        QStringList log_value_enabled;

        QStringList log_values_names_sorted;
        QStringList log_values_by_protocol;

        QStringList dashboard_log_value_id;
        QStringList lower_panel_log_value_id;
        QString logging_values_protocol;

        // Switch values
        QStringList log_switch_protocol;
        QStringList log_switch_id;
        QStringList log_switch_name;
        QStringList log_switch_description;
        QStringList log_switch_address;
        QStringList log_switch_ecu_byte_index;
        QStringList log_switch_ecu_bit;
        QStringList log_switch_target;
        QStringList log_switch_enabled;
        QStringList log_switch_state;

        QStringList log_switches_names_sorted;

        QStringList lower_panel_switch_id;
    } LogValuesStruct;

    struct dt_codes_structure
    {
        QStringList dt_code_id;
        QStringList dt_code_name;
        QStringList dt_code_description;
        QStringList dt_code_temp_address;
        QStringList dt_code_mem_address;
        QStringList dt_code_ecu_bit;
    } dt_codes_struct;

    struct EcuCalDefStructure
    {
        QString FileName;
        QString DefinitionFileName;
        QString FullFileName;
        QString FileSize;
        QStringList IdList;
        QStringList TypeList;
        QStringList NameList;
        QStringList AddressList;
        QStringList CategoryList;
        QStringList CategoryExpandedList;
        QStringList SubCategoryList;
        QStringList LevelList;
        QStringList UserLevelList;
        QStringList SwapXYList;
        QStringList FlipXList;
        QStringList FlipYList;
        QStringList XSizeList;
        QStringList YSizeList;
        QStringList StartPosList;
        QStringList IntervalList;
        QStringList MinValueList;
        QStringList MaxValueList;
        QStringList UnitsList;
        QStringList FormatList;
        QStringList FineIncList;
        QStringList CoarseIncList;
        QStringList VisibleList;
        QStringList SelectionsNameList;
        QStringList SelectionsValueList;
        QStringList DescriptionList;
        QStringList StateList;
        QStringList MapScalingNameList;
        QStringList MapData;
        QStringList MapCellColorMin;
        QStringList MapCellColorMax;

        QStringList XScaleTypeList;
        QStringList XScaleNameList;
        QStringList XScaleAddressList;
        QStringList XScaleStartPosList;
        QStringList XScaleIntervalList;
        QStringList XScaleMinValueList;
        QStringList XScaleMaxValueList;
        QStringList XScaleUnitsList;
        QStringList XScaleFormatList;
        QStringList XScaleFineIncList;
        QStringList XScaleCoarseIncList;
        QStringList XScaleStorageTypeList;
        QStringList XScaleEndianList;
        QStringList XScaleLogParamList;
        QStringList XScaleFromByteList;
        QStringList XScaleToByteList;
        QStringList XScaleStaticDataList;
        QStringList XScaleScalingNameList;
        QStringList XScaleData;

        QStringList YScaleTypeList;
        QStringList YScaleNameList;
        QStringList YScaleAddressList;
        QStringList YScaleStartPosList;
        QStringList YScaleIntervalList;
        QStringList YScaleMinValueList;
        QStringList YScaleMaxValueList;
        QStringList YScaleUnitsList;
        QStringList YScaleFormatList;
        QStringList YScaleFineIncList;
        QStringList YScaleCoarseIncList;
        QStringList YScaleStorageTypeList;
        QStringList YScaleEndianList;
        QStringList YScaleLogParamList;
        QStringList YScaleFromByteList;
        QStringList YScaleToByteList;
        QStringList YScaleScalingNameList;
        QStringList YScaleData;

        QStringList ScalingNameList;
        QStringList ScalingUnitsList;
        QStringList ScalingFromByteList;
        QStringList ScalingToByteList;
        QStringList ScalingFormatList;
        QStringList ScalingMinValueList;
        QStringList ScalingMaxValueList;
        QStringList ScalingCoarseIncList;
        QStringList ScalingFineIncList;
        QStringList ScalingStorageTypeList;
        QStringList ScalingEndianList;
        QStringList ScalingSelectionsNameList;
        QStringList ScalingSelectionsValueList;

        QStringList RomInfo;
        QString RomInfoExpanded;
        QString RomBase;
        QString RomId;
        QString Kernel;
        QString KernelStartAddr;
        QString FlashMethod;
        QString McuType;

        QStringList StorageTypeList;
        QStringList EndianList;
        QStringList LogParamList;
        QStringList FromByteList;
        QStringList ToByteList;
        QStringList MapDefined;

        QByteArray FullRomData;
        bool OemEcuFile;
        bool SyncedWithEcu;
        bool use_romraider_definition;
        bool use_ecuflash_definition;

        QStringList RomInfoStrings = {
            "XML ID",
            "Internal ID Address",
            "Internal ID String",
            "ECU ID",
            "Make",
            "Market",
            "Model",
            "Submodel",
            "Transmission",
            "Year",
            "Flash Method",
            "Memory Model",
            "Checksum Module",
            "Rom Base",
            "File Size",
            "Def File",
        };

        QStringList RomInfoNames = {
            "xmlid",
            "internalidaddress",
            "internalidstring",
            "ecuid",
            "make",
            "market",
            "model",
            "submodel",
            "transmission",
            "year",
            "flashmethod",
            "memmodel",
            "checksummodule",
            "rombase",
            "filesize",
            "deffile",
        };

        QStringList DefHeaderStrings = {
            "XML ID",
            "Internal ID Address",
            "Internal ID String",
            "ECU ID",
            "Make",
            "Market",
            "Model",
            "Submodel",
            "Transmission",
            "Year",
            "Flash Method",
            "Memory Model",
            "Checksum Module",
            "Include",
            "Notes",
        };

        QStringList DefHeaderNames = {
            "xmlid",
            "internalidaddress",
            "internalidstring",
            "ecuid",
            "make",
            "market",
            "model",
            "submodel",
            "transmission",
            "year",
            "flashmethod",
            "memmodel",
            "checksummodule",
            "include",
            "notes",
        };

    } EcuCalDefStruct;

    // EcuCalDefStructure *ecuCalDefTemp;

    enum RomInfoEnum
    {
        XmlId,
        InternalIdAddress,
        InternalIdString,
        EcuId,
        Make,
        Market,
        Model,
        SubModel,
        Transmission,
        Year,
        FlashMethod,
        MemModel,
        ChecksumModule,
        RomBase,
        FileSize,
        DefFile,
    };

    /***********************************
     * Negative response codes (NRC)
     * ********************************/
    static const QHash<int, QString> neg_rsp_codes;   // Inited at error_codes.h
    static const QHash<int, QString> dtc_Pxxxx_codes; // Inited at error_codes.h
    static const QHash<int, QString> dtc_Bxxxx_codes; // Inited at error_codes.h
    static const QHash<int, QString> dtc_Cxxxx_codes; // Inited at error_codes.h
    static const QHash<int, QString> dtc_Uxxxx_codes; // Inited at error_codes.h

    /****************************************************
     * Check if FastECU dir exists in users home folder
     * If not, create one with appropriate files
     ***************************************************/
    ConfigValuesStructure *set_base_dirs(ConfigValuesStructure *configValues,
                                         const fastecu::config::AppRootInfo& root_info);
    ConfigValuesStructure *check_config_dirs(ConfigValuesStructure *configValues);

    /****************************
     * Read FastECU config file
     ***************************/
    ConfigValuesStructure *read_config_file(ConfigValuesStructure *configValues);

    /****************************
     * Save FastECU config file
     ***************************/
    ConfigValuesStructure *save_config_file(FileActions::ConfigValuesStructure *configValues);

    /*************************************
     * Read FastECU flash protocols file
     ************************************/
    ConfigValuesStructure *read_protocols_file(FileActions::ConfigValuesStructure *configValues);
    static bool validate_flash_protocols(const ConfigValuesStructure& configValues, QStringList *errors = nullptr);
    static bool validate_logger_values(const LogValuesStructure& logValues, QStringList *errors = nullptr);
    static bool validate_logger_switches(const LogValuesStructure& logValues, QStringList *errors = nullptr);
    static bool validate_calibration_maps(const EcuCalDefStructure& ecuCalDef, QStringList *errors = nullptr);
    static QStringList collect_ecuflash_base_header_fields(const EcuCalDefStructure& ecuCalDef,
                                                           const QStringList& defData,
                                                           int *endIndex = nullptr);
    static QStringList collect_ecuflash_definition_body_lines(const QStringList& defData, int startIndex);

    /************************
     * Read logger def file
     ***********************/
    LogValuesStructure *read_logger_definition_file();

    /*************************
     * Read logger conf file
     ************************/
    LogValuesStructure *read_logger_conf(FileActions::LogValuesStructure *logValues, const QString& ecu_id, bool modify);

    /************************
     * Save logger conf file
     ************************/
    void *save_logger_conf(FileActions::LogValuesStructure *logValues, QString ecu_id);

    /*****************************************************
     * Search and read RomRaider ECU definition from file
     *****************************************************/
    ConfigValuesStructure *create_romraider_def_id_list(ConfigValuesStructure *configValues);
    EcuCalDefStructure *read_romraider_ecu_base_def(FileActions::EcuCalDefStructure *ecuCalDef);
    EcuCalDefStructure *read_romraider_ecu_def(FileActions::EcuCalDefStructure *ecuCalDef, const QString& ecuId);
    EcuCalDefStructure *add_romraider_def_list_item(EcuCalDefStructure *ecuCalDef);

    /*****************************************************
     * Search and read RomRaider ECU definition from file
     *****************************************************/
    QString convert_value_format(const QString& value_format);
    ConfigValuesStructure *create_ecuflash_def_id_list(ConfigValuesStructure *configValues);
    // EcuCalDefStructure *read_ecuflash_ecu_base_def(FileActions::EcuCalDefStructure *ecuCalDef);
    EcuCalDefStructure *read_ecuflash_ecu_def(FileActions::EcuCalDefStructure *ecuCalDef, const QString& cal_id);
    EcuCalDefStructure *parse_ecuflash_def_scalings(EcuCalDefStructure *ecuCalDef);
    EcuCalDefStructure *add_ecuflash_def_list_item(EcuCalDefStructure *ecuCalDef);
    QString parse_strict_bool_attribute(const QDomElement& element, const QString& attrName, const QString& tableName);

    // EcuCalDefStructure *read_ecuflash_ecu_def_test(FileActions::EcuCalDefStructure *ecuCalDef, QString cal_id);

    QString parse_hex_ecuid(uint8_t byte);
    EcuCalDefStructure *parse_ecuid_ecuflash_def_files(FileActions::EcuCalDefStructure *ecuCalDef, bool is_ascii);
    EcuCalDefStructure *parse_ecuid_romraider_def_files(FileActions::EcuCalDefStructure *ecuCalDef, bool is_ascii);

    EcuCalDefStructure *create_new_definition_for_rom(FileActions::EcuCalDefStructure *ecuCalDef);
    EcuCalDefStructure *use_existing_definition_for_rom(FileActions::EcuCalDefStructure *ecuCalDef);

    /***********************************************
     * Open ECU ROM file, including possible
     * checksum calculations and value conversions
     **********************************************/
    EcuCalDefStructure *open_subaru_rom_file(FileActions::EcuCalDefStructure *ecuCalDef, QString fileName);

    /***********************************************
     * Save ECU ROM file, including possible
     * checksum calculations and value conversions
     **********************************************/
    EcuCalDefStructure *save_subaru_rom_file(FileActions::EcuCalDefStructure *ecuCalDef, const QString& fileName);

    /***************************
     * Read software menu file
     * for menu creation
     **************************/
    QSignalMapper *read_menu_file(QMenuBar *menubar, QToolBar *toolBar);

    /***************************
     * Calculate Subaru 32-bit
     * checksums
     **************************/
    EcuCalDefStructure *checksum_correction(FileActions::EcuCalDefStructure *ecuCalDef);

    /*************************************
     * Parse expression strings for used
     * in ROM map data conversion
     ************************************/
    QStringList parse_stringlist_from_expression_string(const QString& expression, const QString& x);

    /**************************************************
     * Calculate ROM map data with parsed expressions
     *************************************************/
    double calculate_value_from_expression(QStringList expression);

    /**************************************************
     * Parse negative response code message
     *************************************************/
    static QString parse_nrc_message(const QByteArray& nrc);
    /**************************************************
     * Parse diagnostic trouble code message
     *************************************************/
    static QString parse_dtc_message(uint16_t dtc);

  private:
    fastecu::config::LegacyConfigAdapter configAdapter_;
    fastecu::checksum::LegacyChecksumAdapter checksumAdapter_;

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);
};
