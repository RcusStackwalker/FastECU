#pragma once

#include <QApplication>
#include <QWidget>
#include <QScreen>
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
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QTextEdit>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/definitions/config_values.h"
#include "src/backend/definitions/ecu_cal_def.h"
#include "src/backend/definitions/log_values.h"
#include "src/backend/calibration/legacy_calibration_adapter.h"
#include "src/backend/config/legacy_config_adapter.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/definition/legacy_definition_adapter.h"
#include "src/backend/ports/atomic_file_writer.h"
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
                fastecu::IFileRepository& file_repository,
                fastecu::IAtomicFileWriter& atomic_file_writer,
                QWidget *parent = nullptr);

    uint8_t float_precision = 15;
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

    // Defined in log_values.h (see that file's comment for why it is not a
    // nested struct here anymore) and re-exposed under its historical name
    // so every existing `FileActions::LogValuesStructure` call site keeps
    // compiling unchanged.
    using LogValuesStructure = fastecu::definitions::LogValuesStructure;
    LogValuesStructure LogValuesStruct;

    using EcuCalDefStructure = fastecu::definitions::EcuCalDefStructure;
    EcuCalDefStructure EcuCalDefStruct;

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

    /*****************************************************
     * Search and read RomRaider ECU definition from file
     *****************************************************/
    ConfigValuesStructure *create_romraider_def_id_list(ConfigValuesStructure *configValues);
    EcuCalDefStructure *read_romraider_ecu_base_def(FileActions::EcuCalDefStructure *ecuCalDef);
    EcuCalDefStructure *read_romraider_ecu_def(FileActions::EcuCalDefStructure *ecuCalDef, const QString& ecuId);

    /*****************************************************
     * Search and read RomRaider ECU definition from file
     *****************************************************/
    ConfigValuesStructure *create_ecuflash_def_id_list(ConfigValuesStructure *configValues);
    // EcuCalDefStructure *read_ecuflash_ecu_base_def(FileActions::EcuCalDefStructure *ecuCalDef);
    EcuCalDefStructure *read_ecuflash_ecu_def(FileActions::EcuCalDefStructure *ecuCalDef, const QString& cal_id);

    // EcuCalDefStructure *read_ecuflash_ecu_def_test(FileActions::EcuCalDefStructure *ecuCalDef, QString cal_id);

    QString parse_hex_ecuid(uint8_t byte);
    EcuCalDefStructure *parse_ecuid_ecuflash_def_files(FileActions::EcuCalDefStructure *ecuCalDef, bool is_ascii);
    EcuCalDefStructure *parse_ecuid_romraider_def_files(FileActions::EcuCalDefStructure *ecuCalDef, bool is_ascii);

    EcuCalDefStructure *create_new_definition_for_rom(FileActions::EcuCalDefStructure *ecuCalDef);
    EcuCalDefStructure *use_existing_definition_for_rom(FileActions::EcuCalDefStructure *ecuCalDef);

    /*******************************************************************
     * Placeholder RomInfo fields for a ROM the user chose to open
     * without a definition file. Only the caller that owns the
     * chooser dialog (MainWindow::prompt_for_missing_definition) knows
     * whether that choice was made, so this is deliberately not
     * applied by open_subaru_rom_file itself.
     ******************************************************************/
    void apply_missing_definition_defaults(FileActions::EcuCalDefStructure *ecuCalDef);

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

    /*************************************
     * Parse expression strings for used
     * in ROM map data conversion
     ************************************/
    QStringList parse_stringlist_from_expression_string(const QString& expression, const QString& x);

    /**************************************************
     * Calculate ROM map data with parsed expressions
     *************************************************/
    double calculate_value_from_expression(const QStringList& expression);

    /**************************************************
     * Parse negative response code message
     *************************************************/
    static QString parse_nrc_message(const QByteArray& nrc);
    /**************************************************
     * Parse diagnostic trouble code message
     *************************************************/
    static QString parse_dtc_message(uint16_t dtc);

  private:
    friend class TestFileActionsParsing;

    fastecu::Status submit_new_definition(
        std::string_view destination,
        const fastecu::definition::DefinitionHeaderInput&);
    fastecu::Status submit_imported_definition(
        std::string_view source, std::string_view destination,
        const fastecu::definition::DefinitionHeaderInput&);
    void remember_submitted_ecuflash_handle(
        std::string_view destination);
    fastecu::Result<fastecu::definition::DefinitionCatalog> build_definition_catalog(
        fastecu::definition::DefinitionFormat format);
    QString definition_source(
        fastecu::definition::DefinitionFormat format,
        const QString& id) const;
    void log_definition_error(
        const QString& operation,
        const fastecu::Error& error);
    fastecu::Status load_configured_definition(
        EcuCalDefStructure& ecu_cal_def,
        fastecu::definition::DefinitionFormat format,
        const QString& definition_id);
    // The definition load_configured_definition last resolved successfully,
    // or nullptr when that was for a different format/id (or never happened).
    const fastecu::definition::RomDefinition *resolved_definition(
        fastecu::definition::DefinitionFormat format,
        const QString& definition_id) const;
    bool log_definition_load_failure(
        const QString& operation,
        const fastecu::Error& error,
        const QString& source,
        const QString& warning_title,
        const QString& warning_text);
    static void strip_legacy_address_prefixes(QStringList& addresses);
    void apply_flash_method_alias(EcuCalDefStructure& ecuCalDef);
    void normalize_definition_addresses(EcuCalDefStructure& ecuCalDef);

    fastecu::config::LegacyConfigAdapter configAdapter_;
    fastecu::IFileSystem& definitionFileSystem_;
    fastecu::IFileRepository& definitionFileRepository_;
    fastecu::IResourceBundle& loggerResourceBundle_;
    fastecu::IAtomicFileWriter& loggerAtomicFileWriter_;
    fastecu::definition::DefinitionService definitionService_;
    fastecu::definition::LegacyDefinitionAdapter definitionAdapter_;
    struct ResolvedDefinition
    {
        fastecu::definition::DefinitionFormat format;
        QString id;
        fastecu::definition::RomDefinition definition;
    };
    std::optional<ResolvedDefinition> resolvedDefinition_;
    std::vector<std::string> submittedEcuflashHandles_;
    // Declared last so that definitionFileRepository_ -- its constructor
    // argument -- is already initialized: C++ initializes members in
    // declaration order regardless of initializer-list order.
    fastecu::calibration::LegacyCalibrationAdapter calibrationAdapter_;

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);
};
