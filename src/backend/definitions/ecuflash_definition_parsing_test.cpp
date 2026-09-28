#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"

namespace
{
class CountingFileRepository : public fastecu::IFileRepository
{
  public:
    fastecu::Result<std::vector<std::uint8_t>> read(std::string_view handle) override
    {
        ++readCount;
        ++readCounts[std::string(handle)];
        return repository.read(handle);
    }

    fastecu::Status write(std::string_view handle, std::span<const std::uint8_t> data) override
    {
        return repository.write(handle, data);
    }

    int readCount{0};
    std::map<std::string, int> readCounts;

  private:
    QtFileRepository repository;
};

int logCountAt(const fastecu::RecordingEventSink& sink, fastecu::LogLevel level)
{
    return static_cast<int>(
        std::ranges::count_if(sink.logs, [level](const auto& entry) { return entry.first == level; }));
}

} // namespace

class TestEcuflashDefinitionParsing : public QObject
{
    Q_OBJECT
  private slots:
    // A fresh session per test: settings edits must not leak between cases.
    void init()
    {
        config_ = std::make_unique<fastecu::config::testing::ConfigSessionFixture>();
        QVERIFY(config_->initialize().has_value());
    }

    void malformed_ecuflash_catalog_file_is_skipped_and_replaces_with_empty_catalog()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString definitionPath = writeDefFile(dir, "BROKEN", "<rom><romid>");
        QVERIFY(!definitionPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink,
                                config_->session);
        config_->session.settings().ecuflash_definition_files_directory = dir.path().toStdString();
        auto& indexes = fileActions.definitionIndexes;
        indexes.ecuflash_def_cal_id = {"sentinel-id"};
        indexes.ecuflash_def_cal_id_addr = {"sentinel-address"};
        indexes.ecuflash_def_ecu_id = {"sentinel-ecu"};
        indexes.ecuflash_def_filename = {"sentinel-source"};

        fileActions.create_ecuflash_def_id_list();

        // A malformed file in the configured directory is skipped rather than treated as
        // fatal (see DefinitionService::build_catalog), so this replace succeeds with an
        // empty catalog instead of preserving the old rows.
        QVERIFY(indexes.ecuflash_def_cal_id.isEmpty());
        QVERIFY(indexes.ecuflash_def_cal_id_addr.isEmpty());
        QVERIFY(indexes.ecuflash_def_ecu_id.isEmpty());
        QVERIFY(indexes.ecuflash_def_filename.isEmpty());
        QCOMPARE(logCountAt(eventSink, fastecu::LogLevel::Error), 0);
    }

    void removed_configured_definition_is_not_retried_on_refresh()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const QString definitionPath =
            writeDefFileAt(workspace.filePath("removed.xml"), "<rom><romid><xmlid>REMOVED_XML</xmlid>"
                                                              "<internalidaddress>10</internalidaddress>"
                                                              "<internalidstring>REMOVED_INTERNAL</internalidstring>"
                                                              "</romid></rom>");
        QVERIFY(!definitionPath.isEmpty());

        fastecu::RecordingEventSink eventSink;
        FileActions fileActions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink,
                                config_->session);
        config_->session.settings().ecuflash_definition_files_directory = workspace.path().toStdString();

        fileActions.create_ecuflash_def_id_list();
        QCOMPARE(fileActions.definitionIndexes.ecuflash_def_cal_id, QStringList({"REMOVED_XML"}));
        QCOMPARE(fileRepository_.readCounts.at(definitionPath.toStdString()), 1);
        QVERIFY(QFile::remove(definitionPath));

        fileActions.create_ecuflash_def_id_list();

        QVERIFY(logCountAt(eventSink, fastecu::LogLevel::Error) == 0);
        QVERIFY(fileActions.definitionIndexes.ecuflash_def_cal_id.isEmpty());
        QVERIFY(fileActions.definitionIndexes.ecuflash_def_cal_id_addr.isEmpty());
        QVERIFY(fileActions.definitionIndexes.ecuflash_def_ecu_id.isEmpty());
        QVERIFY(fileActions.definitionIndexes.ecuflash_def_filename.isEmpty());
        QCOMPARE(fileRepository_.readCounts.at(definitionPath.toStdString()), 1);
    }

  private:
    static QString writeDefFileAt(const QString& path, const QString& xml)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            return {};
        }
        const QByteArray contents = xml.toUtf8();
        if (file.write(contents) != contents.size())
        {
            return {};
        }
        file.close();
        return path;
    }

    static QString writeDefFile(const QTemporaryDir& dir, const QString& baseName, const QString& xml)
    {
        const QString path = dir.filePath(baseName + ".xml");
        return writeDefFileAt(path, xml);
    }

    // FileActions's constructor takes the file ports and the configuration
    // session; the parsing paths this test exercises only read settings, so
    // plain Qt ports and an in-memory session are sufficient.
    std::unique_ptr<fastecu::config::testing::ConfigSessionFixture> config_;
    QtFileSystem fileSystem_;
    QtResourceBundle resourceBundle_;
    CountingFileRepository fileRepository_;
    fastecu::InMemoryAtomicFileWriter atomicFileWriter_;
};

QTEST_APPLESS_MAIN(TestEcuflashDefinitionParsing)
#include "ecuflash_definition_parsing_test.moc"
