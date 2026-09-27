#include <QApplication>
#include <QCheckBox>
#include <QGroupBox>
#include <QListWidget>
#include <QPushButton>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "src/backend/config/config_session.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/ui/desktop/settings.h"

namespace
{

// Accepts every message box that appears while it lives, recording its text.
class ModalCollector
{
  public:
    ModalCollector()
    {
        QObject::connect(&timer_, &QTimer::timeout,
                         [this]
                         {
                             if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                             {
                                 texts_.append(box->text());
                                 box->accept();
                             }
                         });
        timer_.start(10);
    }
    const QStringList& texts() const
    {
        return texts_;
    }

  private:
    QTimer timer_;
    QStringList texts_;
};

struct SessionOnDisk
{
    explicit SessionOnDisk(const QString& root)
    {
        status = session.initialize(root.toStdString(), "0.1.0-beta.5");
    }
    QtFileSystem file_system;
    QtResourceBundle resource_bundle;
    QtFileRepository file_repository;
    QtEventSink events;
    fastecu::config::ConfigSession session{file_system, resource_bundle, file_repository, events};
    fastecu::Status status;
};

} // namespace

class SettingsTest : public QObject
{
    Q_OBJECT

  private slots:
    void providerCheckboxesPersistBothDirections_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::newRow("enabled-romraider") << true;
        QTest::newRow("disabled-ecuflash") << false;
    }

    void providerCheckboxesPersistBothDirections()
    {
        QFETCH(bool, enabled);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        SessionOnDisk disk{root.path()};
        QVERIFY(disk.status.has_value());
        {
            Settings settings{disk.session};
            const auto boxes = settings.findChildren<QCheckBox *>();
            QCOMPARE(boxes.size(), 3);
            for (auto *box : boxes)
            {
                box->setChecked(!enabled);
                box->click();
                QCOMPARE(box->isChecked(), enabled);
            }
            QCOMPARE(disk.session.settings().use_romraider_definitions, std::string(enabled ? "enabled" : "disabled"));
            QCOMPARE(disk.session.settings().use_ecuflash_definitions, std::string(enabled ? "enabled" : "disabled"));
            QCOMPARE(disk.session.settings().primary_definition_base, std::string(enabled ? "romraider" : "ecuflash"));
        }
        SessionOnDisk reread{root.path()};
        QVERIFY(reread.status.has_value());
        QCOMPARE(reread.session.settings().use_romraider_definitions,
                 disk.session.settings().use_romraider_definitions);
        QCOMPARE(reread.session.settings().use_ecuflash_definitions, disk.session.settings().use_ecuflash_definitions);
        QCOMPARE(reread.session.settings().primary_definition_base, disk.session.settings().primary_definition_base);
    }

    void removingDefinitionsPreservesOrderAndPersistsEmptyList_data()
    {
        QTest::addColumn<bool>("remove_all");
        QTest::newRow("surviving-order") << false;
        QTest::newRow("empty-list") << true;
    }

    void removingDefinitionsPreservesOrderAndPersistsEmptyList()
    {
        QFETCH(bool, remove_all);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        SessionOnDisk disk{root.path()};
        QVERIFY(disk.status.has_value());
        disk.session.settings().romraider_definition_files = {"/first.xml", "/middle.xml", "/last.xml"};
        {
            Settings settings{disk.session};
            QListWidget *list = nullptr;
            for (auto *candidate : settings.findChildren<QListWidget *>())
            {
                if (candidate->count() == 3 && candidate->item(0)->text() == "/first.xml")
                {
                    list = candidate;
                }
            }
            QVERIFY(list != nullptr);
            QCOMPARE(list->item(1)->text(), QString("/middle.xml"));
            QCOMPARE(list->item(2)->text(), QString("/last.xml"));
            QPushButton *remove = nullptr;
            for (auto *button : settings.findChildren<QPushButton *>())
            {
                if (button->text() == "Remove")
                {
                    remove = button;
                }
            }
            QVERIFY(remove != nullptr);
            list->clearSelection();
            remove->click();
            QCOMPARE(disk.session.settings().romraider_definition_files,
                     (std::vector<std::string>{"/first.xml", "/middle.xml", "/last.xml"}));
            list->setCurrentRow(1);
            remove->click();
            QCOMPARE(disk.session.settings().romraider_definition_files,
                     (std::vector<std::string>{"/first.xml", "/last.xml"}));
            if (remove_all)
            {
                while (list->count() > 0)
                {
                    list->setCurrentRow(0);
                    remove->click();
                }
                QVERIFY(disk.session.settings().romraider_definition_files.empty());
                remove->click();
                QVERIFY(disk.session.settings().romraider_definition_files.empty());
            }
        }
        SessionOnDisk reread{root.path()};
        QVERIFY(reread.status.has_value());
        QCOMPARE(reread.session.settings().romraider_definition_files,
                 disk.session.settings().romraider_definition_files);
    }

    void closingSettingsSavesThroughTheSession()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        SessionOnDisk disk{root.path()};
        QVERIFY(disk.status.has_value());
        const QString config_file = QString::fromStdString(disk.session.provisioned_paths().config_file);
        QVERIFY(QFile::remove(config_file));

        {
            Settings settings{disk.session};
        }

        QVERIFY(QFile::exists(config_file));
    }

    void editsReachTheSessionLive()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        SessionOnDisk disk{root.path()};
        QVERIFY(disk.status.has_value());
        Settings settings{disk.session};

        QVERIFY(QMetaObject::invokeMethod(&settings, "toolbar_iconsize_value_changed", Qt::DirectConnection,
                                          Q_ARG(int, 40)));

        QCOMPARE(disk.session.settings().toolbar_iconsize, std::string("40"));
    }

    void destructionRetriesPersistenceAfterClose()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        SessionOnDisk disk{root.path()};
        QVERIFY(disk.status.has_value());
        const QString config_file = QString::fromStdString(disk.session.provisioned_paths().config_file);
        QVERIFY(QFile::remove(config_file));
        QVERIFY(QDir().mkpath(config_file));
        ModalCollector boxes;
        {
            Settings settings{disk.session};
            settings.close();
            QCOMPARE(boxes.texts().size(), 1);
            QVERIFY(QDir().rmdir(config_file));
            QVERIFY(QMetaObject::invokeMethod(&settings, "toolbar_iconsize_value_changed", Qt::DirectConnection,
                                              Q_ARG(int, 48)));
        }
        QCOMPARE(boxes.texts().size(), 1);
        QFile saved{config_file};
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.readAll().contains(R"(data="48")"));
    }

    void failedSaveKeepsEditsAndWarnsTheOperator()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        SessionOnDisk disk{root.path()};
        QVERIFY(disk.status.has_value());
        const QString config_file = QString::fromStdString(disk.session.provisioned_paths().config_file);
        QVERIFY(QFile::remove(config_file));
        QVERIFY(QDir().mkpath(config_file)); // a directory where the file goes: every write fails

        ModalCollector boxes;
        {
            Settings settings{disk.session};
            QVERIFY(QMetaObject::invokeMethod(&settings, "toolbar_iconsize_value_changed", Qt::DirectConnection,
                                              Q_ARG(int, 40)));
            settings.close();
        }

        QCOMPARE(disk.session.settings().toolbar_iconsize, std::string("40"));
        QCOMPARE(boxes.texts().size(), 1);
        QVERIFY(boxes.texts().front().contains(config_file));
    }
};

QTEST_MAIN(SettingsTest)
#include "settings_test.moc"
