#include <QApplication>
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
