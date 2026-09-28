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
        input.internal_id_address = 0x2AB;
        const fastecu::Status submitted = actions.submit_new_definition(submittedPath.toStdString(), input);
        if (!submitted.has_value())
        {
            QFAIL(submitted.error().detail.c_str());
        }
        QVERIFY(QFile::exists(submittedPath));

        session().settings().ecuflash_definition_files_directory = newDirectory.toStdString();
        actions.create_ecuflash_def_id_list();

        QCOMPARE(actions.definitionIndexes.ecuflash_def_cal_id, QStringList({"NEW_DIRECTORY_XML", "SUBMITTED_XML"}));
        QCOMPARE(actions.definitionIndexes.ecuflash_def_cal_id_addr, QStringList({"30", "2ab"}));
        QCOMPARE(actions.definitionIndexes.ecuflash_def_ecu_id, QStringList({"NEW_DIRECTORY_ECU", "SUBMITTED_ECU"}));
        QCOMPARE(actions.definitionIndexes.ecuflash_def_filename, QStringList({newPath, submittedPath}));
        QVERIFY(!actions.definitionIndexes.ecuflash_def_filename.contains(oldPath));
        using fastecu::definition::DefinitionFormat;
        auto catalog = actions.catalog(DefinitionFormat::EcuFlash);
        QVERIFY(catalog.has_value());
        QCOMPARE(catalog->entries().size(), std::size_t{2});
        QCOMPARE(catalog->entries()[1].source, submittedPath.toStdString());
        QCOMPARE(actions.indexed_source(DefinitionFormat::EcuFlash, "SUBMITTED_XML"),
                 std::optional<std::string>{submittedPath.toStdString()});
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

    // Every test gets a fresh configuration session provisioned in its own
    // temporary root.
    void catalog_scan_failure_preserves_startup_indexes()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink, session());
        session().settings().ecuflash_definition_files_directory = workspace.filePath("missing").toStdString();
        auto& indexes = actions.definitionIndexes;
        indexes.ecuflash_def_cal_id = {"OLD"};
        indexes.ecuflash_def_cal_id_addr = {"abcdef"};
        indexes.ecuflash_def_ecu_id = {"ECU"};
        indexes.ecuflash_def_filename = {"old.xml"};

        actions.create_ecuflash_def_id_list();

        QCOMPARE(indexes.ecuflash_def_cal_id, QStringList{"OLD"});
        QCOMPARE(indexes.ecuflash_def_cal_id_addr, QStringList{"abcdef"});
        QCOMPARE(indexes.ecuflash_def_ecu_id, QStringList{"ECU"});
        QCOMPARE(indexes.ecuflash_def_filename, QStringList{"old.xml"});
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 1);
    }

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
