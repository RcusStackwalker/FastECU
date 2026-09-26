#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "src/backend/definitions/file_actions.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/ui/desktop/settings.h"

class SettingsTest : public QObject
{
    Q_OBJECT

  private slots:
    void closingSettingsSavesTheConfigThroughTheInjectedFileActions()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QtFileSystem file_system;
        QtResourceBundle resource_bundle;
        QtFileRepository file_repository;
        QtAtomicFileWriter file_writer;
        QtEventSink events;
        FileActions file_actions{file_system, resource_bundle, file_repository, file_writer, events};
        FileActions::ConfigValuesStructure *config = &file_actions.ConfigValuesStruct;
        file_actions.set_base_dirs(config, root.path().toStdString());
        file_actions.check_config_dirs(config);
        // check_config_dirs may seed a default config; remove it so the
        // assertion below can only pass if Settings wrote it.
        QFile::remove(config->config_file);
        QVERIFY(!QFile::exists(config->config_file));

        {
            Settings settings{file_actions, config};
        }

        QVERIFY(QFile::exists(config->config_file));
    }
};

QTEST_MAIN(SettingsTest)
#include "settings_test.moc"
