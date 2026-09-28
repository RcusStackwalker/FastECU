#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/config/config_session.h"
#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"

namespace
{
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

QString writeTextFile(const QTemporaryDir& dir, const QString& name, const QByteArray& contents)
{
    return writeTextFileAt(dir.filePath(name), contents);
}

// The minimal RomRaider definition the MINIMAL_TEST case below writes, with
// its <xmlid> text swapped for the argument.
QByteArray romraiderDefinitionWithId(const QString& xmlId)
{
    return ("<roms><rom><romid><xmlid>" + xmlId + "</xmlid></romid></rom></roms>").toUtf8();
}

bool sinkContainsMessage(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level, const QString& text)
{
    return std::ranges::any_of(sink.logs, [&](const auto& entry)
                               { return entry.first == level && QString::fromStdString(entry.second).contains(text); });
}

int logCountAt(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level)
{
    return static_cast<int>(
        std::ranges::count_if(sink.logs, [level](const auto& entry) { return entry.first == level; }));
}
} // namespace

class TestFileActionsParsing : public QObject
{
    Q_OBJECT

  private slots:
    void romraider_definition_indexes_ids_and_inherits_base()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeTextFile(dir, "romraider.xml",
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

        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        session().settings().romraider_definition_files = {definitionPath.toStdString()};
        actions.create_romraider_def_id_list();

        const int idIndex = actions.definitionIndexes.romraider_def_cal_id.indexOf("CAL_TEST");
        QVERIFY(idIndex >= 0);
        QCOMPARE(actions.definitionIndexes.romraider_def_cal_id_addr.at(idIndex), QString("0"));
        QCOMPARE(actions.definitionIndexes.romraider_def_filename.at(idIndex), definitionPath);

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
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Debug, "1 RomRaider definition files found"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Debug, "2 RomRaider ecu id's found"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Debug, "XML ID: CAL_TEST CAL_TEST"));
    }

    void romraider_definition_uses_blank_optional_rom_id_fields()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeTextFile(
            dir, "romraider-minimal.xml", "<roms><rom><romid><xmlid>MINIMAL_TEST</xmlid></romid></rom></roms>");
        QVERIFY(!definitionPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        session().settings().romraider_definition_files = {definitionPath.toStdString()};
        actions.create_romraider_def_id_list();

        const int idIndex = actions.definitionIndexes.romraider_def_cal_id.indexOf("MINIMAL_TEST");
        QVERIFY(idIndex >= 0);
        QCOMPARE(actions.definitionIndexes.romraider_def_cal_id_addr.at(idIndex), QString(""));
        QCOMPARE(actions.definitionIndexes.romraider_def_ecu_id.at(idIndex), QString(""));
        QCOMPARE(actions.definitionIndexes.romraider_def_filename.at(idIndex), definitionPath);

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

    void romraiderDefinitionResolvesFlashMethodAlias()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeTextFile(dir, "romraider_alias.xml",
                                                     R"(<roms>
  <rom>
    <romid>
      <xmlid>ALIAS_TEST</xmlid><internalidaddress>0</internalidaddress>
      <internalidstring>ALIAS_TEST</internalidstring><ecuid>ALIAS_ECU</ecuid>
      <flashmethod>wrx02</flashmethod>
    </romid>
    <table name="Fuel" type="2D" storagetype="uint16" endian="big"
           storageaddress="20" sizex="2" sizey="1">
      <scaling units="%" expression="x*0.5" to_byte="x*2"
               format="0.0" fineincrement="0.5" coarseincrement="1"/>
    </table>
  </rom>
</roms>)");
        QVERIFY(!definitionPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        session().settings().romraider_definition_files = {definitionPath.toStdString()};
        actions.create_romraider_def_id_list();

        // The session's catalog is the bundled protocols.cfg, whose first
        // vehicle with a "wrx02"-aliased protocol uses
        // sub_ecu_denso_mc68hc16y5_02.

        FileActions::EcuCalDefStructure ecu;
        while (ecu.RomInfo.size() < ecu.RomInfoStrings.size())
        {
            ecu.RomInfo.append(" ");
        }
        QCOMPARE(actions.read_romraider_ecu_def(&ecu, "ALIAS_TEST"), &ecu);

        // The invariant Task 7's wraparound deletion depends on: the alias never
        // survives into RomInfo[FlashMethod], so a downstream
        // `RomInfo[FlashMethod] == "wrx02"` comparison can never be true.
        QCOMPARE(ecu.RomInfo.at(FileActions::FlashMethod), QString("sub_ecu_denso_mc68hc16y5_02"));
        QVERIFY(ecu.RomInfo.at(FileActions::FlashMethod) != QString("wrx02"));
    }

    void definition_index_rebuild_keeps_file_order()
    {
        // Two RomRaider definition files; the index lists follow the order
        // of romraider_definition_files, whatever the IDs sort to.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString second = writeTextFile(dir, "b.xml", romraiderDefinitionWithId("ZZZ_FIRST"));
        const QString first = writeTextFile(dir, "a.xml", romraiderDefinitionWithId("AAA_SECOND"));
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        session().settings().romraider_definition_files = {second.toStdString(), first.toStdString()};

        actions.create_romraider_def_id_list();

        QCOMPARE(actions.definitionIndexes.romraider_def_filename, (QStringList{second, first}));
        QCOMPARE(actions.definitionIndexes.romraider_def_cal_id, (QStringList{"ZZZ_FIRST", "AAA_SECOND"}));
    }

    void indexed_source_answers_from_the_startup_indexes()
    {
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        actions.definitionIndexes.ecuflash_def_cal_id = {"ECUFLASH_ID"};
        actions.definitionIndexes.ecuflash_def_filename = {"/defs/ecuflash.xml"};
        actions.definitionIndexes.romraider_def_cal_id = {"ROMRAIDER_ID"};
        actions.definitionIndexes.romraider_def_filename = {"/defs/romraider.xml"};

        using fastecu::definition::DefinitionFormat;
        QCOMPARE(actions.indexed_source(DefinitionFormat::EcuFlash, "ECUFLASH_ID"),
                 std::optional<std::string>("/defs/ecuflash.xml"));
        QCOMPARE(actions.indexed_source(DefinitionFormat::RomRaider, "ROMRAIDER_ID"),
                 std::optional<std::string>("/defs/romraider.xml"));
        QCOMPARE(actions.indexed_source(DefinitionFormat::EcuFlash, "ROMRAIDER_ID"), std::optional<std::string>{});
        QCOMPARE(actions.indexed_source(DefinitionFormat::RomRaider, "UNKNOWN"), std::optional<std::string>{});
    }

    void malformed_romraider_catalog_file_is_skipped_and_replaces_with_empty_catalog()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeTextFile(dir, "malformed-romraider.xml", "<roms><rom>");
        QVERIFY(!definitionPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        session().settings().romraider_definition_files = {definitionPath.toStdString()};
        actions.definitionIndexes.romraider_def_cal_id = {"sentinel-id"};
        actions.definitionIndexes.romraider_def_cal_id_addr = {"sentinel-address"};
        actions.definitionIndexes.romraider_def_ecu_id = {"sentinel-ecu"};
        actions.definitionIndexes.romraider_def_filename = {"sentinel-source"};

        actions.create_romraider_def_id_list();

        // A malformed file among a directory's worth of configured definitions is skipped
        // rather than treated as fatal (see DefinitionService::build_catalog), so this
        // replace succeeds with an empty catalog instead of preserving the old rows.
        QVERIFY(actions.definitionIndexes.romraider_def_cal_id.isEmpty());
        QVERIFY(actions.definitionIndexes.romraider_def_cal_id_addr.isEmpty());
        QVERIFY(actions.definitionIndexes.romraider_def_ecu_id.isEmpty());
        QVERIFY(actions.definitionIndexes.romraider_def_filename.isEmpty());
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 0);
    }

    void malformed_romraider_definition_reports_definition_not_found()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeTextFile(dir, "malformed-romraider.xml", "<roms><rom>");
        QVERIFY(!definitionPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        session().settings().romraider_definition_files = {definitionPath.toStdString()};
        actions.definitionIndexes.romraider_def_cal_id = {"BROKEN"};
        actions.definitionIndexes.romraider_def_cal_id_addr = {"0"};
        actions.definitionIndexes.romraider_def_ecu_id = {"sentinel-ecu"};
        actions.definitionIndexes.romraider_def_filename = {definitionPath};

        FileActions::EcuCalDefStructure ecu;
        ecu.RomInfo = QStringList(ecu.RomInfoStrings.size(), "sentinel-rom-info");
        ecu.DefinitionFileName = "sentinel-definition-file";
        ecu.NameList = {"sentinel-map"};
        ecu.use_romraider_definition = false;
        const QStringList romInfo = ecu.RomInfo;
        const QString definitionFileName = ecu.DefinitionFileName;
        const QStringList names = ecu.NameList;

        QCOMPARE(actions.read_romraider_ecu_def(&ecu, "BROKEN"), &ecu);

        QCOMPARE(ecu.RomInfo, romInfo);
        QCOMPARE(ecu.DefinitionFileName, definitionFileName);
        QCOMPARE(ecu.NameList, names);
        QVERIFY(!ecu.use_romraider_definition);
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        // The malformed file is skipped while building the catalog (see
        // DefinitionService::build_catalog), so "BROKEN" is simply absent from it rather than
        // failing with a parse error.
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "definition ID not found"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "BROKEN"));
    }

    void romraider_base_missing_source_logs_context_and_preserves_state()
    {
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        FileActions::EcuCalDefStructure ecu;
        ecu.RomInfo = QStringList(ecu.RomInfoStrings.size(), "sentinel-rom-info");
        ecu.RomInfo[FileActions::XmlId] = "BASE";
        ecu.NameList = {"sentinel-map"};
        const FileActions::EcuCalDefStructure original = ecu;

        QCOMPARE(actions.read_romraider_ecu_base_def(&ecu), nullptr);

        QVERIFY(ecu == original);
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "RomRaider base definition"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "source"));
    }

    void romraider_base_missing_definition_id_logs_context_and_preserves_state()
    {
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        FileActions::EcuCalDefStructure ecu;
        ecu.DefinitionFileName = "base.xml";
        ecu.RomInfo = QStringList(ecu.RomInfoStrings.size(), " ");
        ecu.NameList = {"sentinel-map"};
        const FileActions::EcuCalDefStructure original = ecu;

        QCOMPARE(actions.read_romraider_ecu_base_def(&ecu), nullptr);

        QVERIFY(ecu == original);
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "RomRaider base definition"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "definition ID"));
    }

    void missing_romraider_base_returns_null_and_preserves_caller_state()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        FileActions::EcuCalDefStructure ecu;
        ecu.RomInfo = QStringList(ecu.RomInfoStrings.size(), "sentinel-rom-info");
        ecu.DefinitionFileName = dir.filePath("missing-romraider.xml");
        ecu.NameList = {"sentinel-map"};
        const QStringList romInfo = ecu.RomInfo;
        const QString definitionFileName = ecu.DefinitionFileName;
        const QStringList names = ecu.NameList;

        QCOMPARE(actions.read_romraider_ecu_base_def(&ecu), nullptr);

        QCOMPARE(ecu.RomInfo, romInfo);
        QCOMPARE(ecu.DefinitionFileName, definitionFileName);
        QCOMPARE(ecu.NameList, names);
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "cannot open file"));
        QVERIFY(!eventSink.notices.empty());
        QVERIFY(
            QString::fromStdString(eventSink.notices.front()).contains("Unable to open OEM ecu base definitions file"));
    }

    void malformed_compatibility_catalog_columns_preserve_rom_id()
    {
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        actions.definitionIndexes.romraider_def_cal_id = {"AB10"};
        actions.definitionIndexes.romraider_def_cal_id_addr = {"0", "1"};
        actions.definitionIndexes.romraider_def_ecu_id = {};
        actions.definitionIndexes.romraider_def_filename = {"hex-definition.xml"};

        FileActions::EcuCalDefStructure ecu;
        ecu.RomId = "sentinel-rom-id";
        ecu.FullRomData = QByteArray::fromHex("AB10");

        QCOMPARE(actions.parse_ecuid_romraider_def_files(&ecu, false), &ecu);

        QCOMPARE(ecu.RomId, QString("sentinel-rom-id"));
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "ID/source/address/ECU"));
    }

    void directory_change_drops_discovered_sources_and_keeps_successful_submission()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const QString oldDirectory = workspace.filePath("old");
        const QString newDirectory = workspace.filePath("new");
        QVERIFY(QDir().mkpath(oldDirectory));
        QVERIFY(QDir().mkpath(newDirectory));
        const QString oldPath =
            writeTextFileAt(oldDirectory + "/old.xml", "<rom><romid><xmlid>OLD_DIRECTORY_XML</xmlid>"
                                                       "<internalidaddress>10</internalidaddress>"
                                                       "<internalidstring>OLD_DIRECTORY_INTERNAL</internalidstring>"
                                                       "<ecuid>OLD_DIRECTORY_ECU</ecuid></romid></rom>");
        const QString newPath =
            writeTextFileAt(newDirectory + "/new.xml", "<rom><romid><xmlid>NEW_DIRECTORY_XML</xmlid>"
                                                       "<internalidaddress>30</internalidaddress>"
                                                       "<internalidstring>NEW_DIRECTORY_INTERNAL</internalidstring>"
                                                       "<ecuid>NEW_DIRECTORY_ECU</ecuid></romid></rom>");
        const QString submittedPath = workspace.filePath("submitted.xml");
        QVERIFY(!oldPath.isEmpty());
        QVERIFY(!newPath.isEmpty());

        QtAtomicFileWriter writer;
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, writer, eventSink, session());
        session().settings().ecuflash_definition_files_directory = oldDirectory.toStdString();
        actions.create_ecuflash_def_id_list();
        QCOMPARE(actions.definitionIndexes.ecuflash_def_cal_id, QStringList({"OLD_DIRECTORY_XML"}));

        auto input = validHeaderInput();
        input.xml_id = "SUBMITTED_XML";
        input.internal_id = "SUBMITTED_INTERNAL";
        input.ecu_id = "SUBMITTED_ECU";
        input.internal_id_address = 0x20;
        const fastecu::Status submitted = actions.submit_new_definition(submittedPath.toStdString(), input);
        if (!submitted.has_value())
        {
            QFAIL(submitted.error().detail.c_str());
        }
        QVERIFY(QFile::exists(submittedPath));

        session().settings().ecuflash_definition_files_directory = newDirectory.toStdString();
        actions.create_ecuflash_def_id_list();

        QCOMPARE(actions.definitionIndexes.ecuflash_def_cal_id, QStringList({"NEW_DIRECTORY_XML", "SUBMITTED_XML"}));
        QCOMPARE(actions.definitionIndexes.ecuflash_def_cal_id_addr, QStringList({"30", "20"}));
        QCOMPARE(actions.definitionIndexes.ecuflash_def_ecu_id, QStringList({"NEW_DIRECTORY_ECU", "SUBMITTED_ECU"}));
        QCOMPARE(actions.definitionIndexes.ecuflash_def_filename, QStringList({newPath, submittedPath}));
        QVERIFY(!actions.definitionIndexes.ecuflash_def_filename.contains(oldPath));
    }

    void successful_submission_provenance_is_sorted_and_deduplicated()
    {
        atomicFileWriter_.reset();
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        const auto input = validHeaderInput();

        QVERIFY(actions.submit_new_definition("z.xml", input));
        QVERIFY(actions.submit_new_definition("a.xml", input));
        QVERIFY(actions.submit_new_definition("z.xml", input));

        QCOMPARE(actions.submittedEcuflashHandles_, (std::vector<std::string>{"a.xml", "z.xml"}));
    }

    void failed_definition_submission_logs_exact_error_and_preserves_catalog()
    {
        atomicFileWriter_.reset();
        const fastecu::Error backendError{
            fastecu::ErrorKind::Disconnected,
            "atomic destination unavailable",
        };
        atomicFileWriter_.replace_error = backendError;
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        auto& indexes = actions.definitionIndexes;
        indexes.ecuflash_def_cal_id = {"sentinel-id"};
        indexes.ecuflash_def_cal_id_addr = {"sentinel-address"};
        indexes.ecuflash_def_ecu_id = {"sentinel-ecu"};
        indexes.ecuflash_def_filename = {"sentinel-source"};
        const QStringList ids = indexes.ecuflash_def_cal_id;
        const QStringList addresses = indexes.ecuflash_def_cal_id_addr;
        const QStringList ecuIds = indexes.ecuflash_def_ecu_id;
        const QStringList sources = indexes.ecuflash_def_filename;

        const fastecu::Status result = actions.submit_new_definition("unavailable.xml", validHeaderInput());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), backendError);
        QCOMPARE(indexes.ecuflash_def_cal_id, ids);
        QCOMPARE(indexes.ecuflash_def_cal_id_addr, addresses);
        QCOMPARE(indexes.ecuflash_def_ecu_id, ecuIds);
        QCOMPARE(indexes.ecuflash_def_filename, sources);
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "Unable to create definition"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "Disconnected"));
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "atomic destination unavailable"));
        QVERIFY(actions.submittedEcuflashHandles_.empty());
    }

    void open_subaru_rom_file_applies_padding_before_size_validation()
    {
        // The ROM-size check must run AFTER the sub_ecu_denso_mc68hc16y5_02
        // padding block, not before it: padding grows FullRomData by 0x8000
        // bytes, so a definition authored against the padded image would be
        // rejected by a check against the pre-padded length. This pins that
        // padding is applied by the time the function returns.
        //
        // It does not reproduce the ordering bug directly -- that needs a
        // real EcuFlash definition fixture with map addresses inside the
        // padded region so definitionService_.load succeeds and validation
        // actually runs. Disproportionate setup for this regression test;
        // recorded here rather than left as an unstated gap.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString romPath = dir.filePath("rom.bin");
        QFile romFile(romPath);
        QVERIFY(romFile.open(QIODevice::WriteOnly));
        // Must be >= 0x20000 (the padding insertion point): QByteArray::insert
        // beyond the current length first grows the array up to that position,
        // so a smaller original would make the observed growth exceed the
        // padding loop's own 0x8000 bytes.
        const QByteArray originalBytes(qsizetype{160} * 1024, '\0');
        QCOMPARE(romFile.write(originalBytes), qint64{originalBytes.size()});
        romFile.close();

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());

        FileActions::EcuCalDefStructure *ecuCalDef = new FileActions::EcuCalDefStructure;
        while (ecuCalDef->RomInfo.length() < ecuCalDef->RomInfoStrings.length())
        {
            ecuCalDef->RomInfo.append(" ");
        }
        ecuCalDef->RomInfo[FileActions::FlashMethod] = "sub_ecu_denso_mc68hc16y5_02";

        ecuCalDef = fileActions.open_subaru_rom_file(ecuCalDef, romPath);

        QVERIFY(ecuCalDef != nullptr);
        QCOMPARE(ecuCalDef->FullRomData.length(), originalBytes.length() + 0x8000);
        delete ecuCalDef;
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

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());

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

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        FileActions::EcuCalDefStructure *ecuCalDef = new FileActions::EcuCalDefStructure;
        while (ecuCalDef->RomInfo.length() < ecuCalDef->RomInfoStrings.length())
        {
            ecuCalDef->RomInfo.append(" ");
        }

        FileActions::EcuCalDefStructure *result =
            fileActions.open_subaru_rom_file(ecuCalDef, dir.filePath("missing.bin"));

        QVERIFY(result == nullptr);
        QVERIFY(!eventSink.notices.empty());
        QVERIFY(
            QString::fromStdString(eventSink.notices.front()).contains("Unable to open calibration file for reading"));
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

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray("\xCA\xFE\xBA\xBE", 4);

        FileActions::EcuCalDefStructure *result = fileActions.save_subaru_rom_file(&ecuCalDef, romPath);

        QVERIFY(result == &ecuCalDef);
        QCOMPARE(ecuCalDef.FullFileName, romPath);
        QCOMPARE(ecuCalDef.FileName, QString("saved.bin"));

        QFile writtenFile(romPath);
        QVERIFY(writtenFile.open(QIODevice::ReadOnly));
        QCOMPARE(writtenFile.readAll(), QByteArray("\xCA\xFE\xBA\xBE", 4));
    }

    void save_subaru_rom_file_warns_and_returns_nullptr_when_write_fails()
    {
        // The only feedback a user gets for a failed ROM save is this warning
        // (both MainWindow call sites historically ignored the return value),
        // so a nullptr from the adapter must still reach the UI.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // Parent directory does not exist -> the write cannot open the file.
        const QString romPath = dir.filePath("no-such-directory/saved.bin");

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.FullRomData = QByteArray("\xCA\xFE", 2);

        FileActions::EcuCalDefStructure *result = fileActions.save_subaru_rom_file(&ecuCalDef, romPath);

        QVERIFY(result == nullptr);
        QVERIFY(sinkContainsMessage(eventSink, fastecu::LogLevel::Error, "for writing"));
        // A failed save must not rewrite the names of the file the ROM came from.
        QVERIFY(ecuCalDef.FullFileName.isEmpty());
    }

    void open_subaru_rom_file_leaves_missing_definition_defaults_to_the_caller()
    {
        // The continue-without-definition placeholders belong to the chooser
        // dialog's continue-without branch (MainWindow), not to every ROM
        // open: a user who picks "create new"/"use existing" must not get
        // them stamped over the definition they just provided.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString romPath = dir.filePath("rom.bin");
        QFile romFile(romPath);
        QVERIFY(romFile.open(QIODevice::WriteOnly));
        QCOMPARE(romFile.write(QByteArray("\x01\x02\x03\x04", 4)), qint64{4});
        romFile.close();

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());

        FileActions::EcuCalDefStructure *ecuCalDef = new FileActions::EcuCalDefStructure;
        while (ecuCalDef->RomInfo.length() < ecuCalDef->RomInfoStrings.length())
        {
            ecuCalDef->RomInfo.append(" ");
        }

        ecuCalDef = fileActions.open_subaru_rom_file(ecuCalDef, romPath);
        QVERIFY(ecuCalDef != nullptr);
        // NOLINTNEXTLINE(clang-analyzer-core.NullDereference) -- QVERIFY above already returns on null
        QVERIFY(!ecuCalDef->use_ecuflash_definition);
        QVERIFY(!ecuCalDef->use_romraider_definition);
        QCOMPARE(ecuCalDef->RomInfo.at(FileActions::XmlId), QString(" "));
        QCOMPARE(ecuCalDef->RomInfo.at(FileActions::Make), QString(" "));

        fileActions.apply_missing_definition_defaults(ecuCalDef);

        QCOMPARE(ecuCalDef->RomInfo.at(FileActions::XmlId), QString("UnknownID"));
        QCOMPARE(ecuCalDef->RomInfo.at(FileActions::InternalIdAddress), QString(""));
        QCOMPARE(ecuCalDef->RomInfo.at(FileActions::InternalIdString), QString(""));
        QCOMPARE(ecuCalDef->RomInfo.at(FileActions::EcuId), QString(""));
        // The selected vehicle's make: opening this ROM matched no protocol,
        // so the session's saved row is unchanged.
        QCOMPARE(ecuCalDef->RomInfo.at(FileActions::Make), QString::fromStdString(session().selected_vehicle()->make));
        QCOMPARE(ecuCalDef->RomInfo.at(FileActions::DefFile), QString(" "));
        // FileSize stays the value open_subaru_rom_file computed; the
        // placeholder block must not recompute it from a padded image.
        QCOMPARE(ecuCalDef->RomInfo.at(FileActions::FileSize), QString("0kb"));
        delete ecuCalDef;
    }

    // Every test gets a fresh configuration session provisioned in its own
    // temporary root.
    void init()
    {
        session_.reset();
        sessionRoot_ = std::make_unique<QTemporaryDir>();
        QVERIFY(sessionRoot_->isValid());
        session_ = std::make_unique<fastecu::config::ConfigSession>(fileSystem_, resourceBundle_, fileRepository_,
                                                                    sessionEvents_);
        const fastecu::Status initialized = session_->initialize(sessionRoot_->path().toStdString(), "test");
        if (!initialized.has_value())
        {
            QFAIL(initialized.error().detail.c_str());
        }
    }

  private:
    fastecu::config::ConfigSession& session()
    {
        return *session_;
    }

    QtFileSystem fileSystem_;
    QtResourceBundle resourceBundle_;
    QtFileRepository fileRepository_;
    fastecu::InMemoryAtomicFileWriter atomicFileWriter_;
    fastecu::RecordingEventSink sessionEvents_;
    std::unique_ptr<QTemporaryDir> sessionRoot_;
    std::unique_ptr<fastecu::config::ConfigSession> session_;
};

QTEST_APPLESS_MAIN(TestFileActionsParsing)

#include "file_actions_parsing_test.moc"
