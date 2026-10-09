#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <string>
#include <QApplication>
#include <QCheckBox>
#include <QGroupBox>
#include <QListWidget>
#include <QPushButton>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <QTimer>

#include "src/backend/config/config_session.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/platform/desktop/common/ports/event_sink/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/ui/desktop/widgets/settings.h"

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
    // The settings dialog reads no vehicle; any consistent catalog will do.
    fastecu::config::ConfigSession session{fastecu::config::testing::kStandardCatalog, file_system, resource_bundle,
                                           file_repository, events};
    fastecu::Status status;
};

} // namespace

struct ProviderCheckboxesPersistBothDirectionsCase
{
    std::string name;
    bool enabled;
};
class ProviderCheckboxesPersistBothDirectionsParameters
    : public ::testing::Test,
      public ::testing::WithParamInterface<ProviderCheckboxesPersistBothDirectionsCase>
{
};

INSTANTIATE_TEST_SUITE_P(Rows, ProviderCheckboxesPersistBothDirectionsParameters,
                         ::testing::Values(ProviderCheckboxesPersistBothDirectionsCase{"enabled_romraider", true},
                                           ProviderCheckboxesPersistBothDirectionsCase{"disabled_ecuflash", false}),
                         [](const ::testing::TestParamInfo<ProviderCheckboxesPersistBothDirectionsCase>& info)
                         { return info.param.name; });

TEST_P(ProviderCheckboxesPersistBothDirectionsParameters, providerCheckboxesPersistBothDirections)
{
    const bool enabled = GetParam().enabled;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionOnDisk disk{root.path()};
    ASSERT_TRUE(disk.status.has_value());
    {
        Settings settings{disk.session};
        const auto boxes = settings.findChildren<QCheckBox *>();
        ASSERT_EQ(boxes.size(), 3);
        for (auto *box : boxes)
        {
            box->setChecked(!enabled);
            box->click();
            ASSERT_EQ(box->isChecked(), enabled);
        }
        ASSERT_EQ(disk.session.settings().use_romraider_definitions, std::string(enabled ? "enabled" : "disabled"));
        ASSERT_EQ(disk.session.settings().use_ecuflash_definitions, std::string(enabled ? "enabled" : "disabled"));
        ASSERT_EQ(disk.session.settings().primary_definition_base, std::string(enabled ? "romraider" : "ecuflash"));
    }
    SessionOnDisk reread{root.path()};
    ASSERT_TRUE(reread.status.has_value());
    ASSERT_EQ(reread.session.settings().use_romraider_definitions, disk.session.settings().use_romraider_definitions);
    ASSERT_EQ(reread.session.settings().use_ecuflash_definitions, disk.session.settings().use_ecuflash_definitions);
    ASSERT_EQ(reread.session.settings().primary_definition_base, disk.session.settings().primary_definition_base);
}

struct RemovingDefinitionsPreservesOrderAndPersistsEmptyListCase
{
    std::string name;
    bool remove_all;
};
class RemovingDefinitionsPreservesOrderAndPersistsEmptyListParameters
    : public ::testing::Test,
      public ::testing::WithParamInterface<RemovingDefinitionsPreservesOrderAndPersistsEmptyListCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, RemovingDefinitionsPreservesOrderAndPersistsEmptyListParameters,
    ::testing::Values(RemovingDefinitionsPreservesOrderAndPersistsEmptyListCase{"surviving_order", false},
                      RemovingDefinitionsPreservesOrderAndPersistsEmptyListCase{"empty_list", true}),
    [](const ::testing::TestParamInfo<RemovingDefinitionsPreservesOrderAndPersistsEmptyListCase>& info)
    { return info.param.name; });

TEST_P(RemovingDefinitionsPreservesOrderAndPersistsEmptyListParameters,
       removingDefinitionsPreservesOrderAndPersistsEmptyList)
{
    const bool removeAll = GetParam().remove_all;
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionOnDisk disk{root.path()};
    ASSERT_TRUE(disk.status.has_value());
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
        ASSERT_TRUE(list != nullptr);
        ASSERT_EQ(list->item(1)->text(), QString("/middle.xml"));
        ASSERT_EQ(list->item(2)->text(), QString("/last.xml"));
        QPushButton *remove = nullptr;
        for (auto *button : settings.findChildren<QPushButton *>())
        {
            if (button->text() == "Remove")
            {
                remove = button;
            }
        }
        ASSERT_TRUE(remove != nullptr);
        list->clearSelection();
        remove->click();
        ASSERT_EQ(disk.session.settings().romraider_definition_files,
                  (std::vector<std::string>{"/first.xml", "/middle.xml", "/last.xml"}));
        list->setCurrentRow(1);
        remove->click();
        ASSERT_EQ(disk.session.settings().romraider_definition_files,
                  (std::vector<std::string>{"/first.xml", "/last.xml"}));
        if (removeAll)
        {
            while (list->count() > 0)
            {
                list->setCurrentRow(0);
                remove->click();
            }
            ASSERT_TRUE(disk.session.settings().romraider_definition_files.empty());
            remove->click();
            ASSERT_TRUE(disk.session.settings().romraider_definition_files.empty());
        }
    }
    SessionOnDisk reread{root.path()};
    ASSERT_TRUE(reread.status.has_value());
    ASSERT_EQ(reread.session.settings().romraider_definition_files, disk.session.settings().romraider_definition_files);
}

TEST(SettingsTest, closingSettingsSavesThroughTheSession)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionOnDisk disk{root.path()};
    ASSERT_TRUE(disk.status.has_value());
    const QString configFile = QString::fromStdString(disk.session.provisioned_paths().config_file);
    ASSERT_TRUE(QFile::remove(configFile));

    {
        Settings settings{disk.session};
    }

    ASSERT_TRUE(QFile::exists(configFile));
}

TEST(SettingsTest, editsReachTheSessionLive)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionOnDisk disk{root.path()};
    ASSERT_TRUE(disk.status.has_value());
    Settings settings{disk.session};

    ASSERT_TRUE(
        QMetaObject::invokeMethod(&settings, "toolbar_iconsize_value_changed", Qt::DirectConnection, Q_ARG(int, 40)));

    ASSERT_EQ(disk.session.settings().toolbar_iconsize, std::string("40"));
}

TEST(SettingsTest, destructionRetriesPersistenceAfterClose)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionOnDisk disk{root.path()};
    ASSERT_TRUE(disk.status.has_value());
    const QString configFile = QString::fromStdString(disk.session.provisioned_paths().config_file);
    ASSERT_TRUE(QFile::remove(configFile));
    ASSERT_TRUE(QDir().mkpath(configFile));
    ModalCollector boxes;
    {
        Settings settings{disk.session};
        settings.close();
        ASSERT_EQ(boxes.texts().size(), 1);
        ASSERT_TRUE(QDir().rmdir(configFile));
        ASSERT_TRUE(QMetaObject::invokeMethod(&settings, "toolbar_iconsize_value_changed", Qt::DirectConnection,
                                              Q_ARG(int, 48)));
    }
    ASSERT_EQ(boxes.texts().size(), 1);
    QFile saved{configFile};
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    ASSERT_TRUE(saved.readAll().contains(R"(data="48")"));
}

TEST(SettingsTest, failedSaveKeepsEditsAndWarnsTheOperator)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionOnDisk disk{root.path()};
    ASSERT_TRUE(disk.status.has_value());
    const QString configFile = QString::fromStdString(disk.session.provisioned_paths().config_file);
    ASSERT_TRUE(QFile::remove(configFile));
    ASSERT_TRUE(QDir().mkpath(configFile)); // a directory where the file goes: every write fails

    ModalCollector boxes;
    {
        Settings settings{disk.session};
        ASSERT_TRUE(QMetaObject::invokeMethod(&settings, "toolbar_iconsize_value_changed", Qt::DirectConnection,
                                              Q_ARG(int, 40)));
        settings.close();
    }

    ASSERT_EQ(disk.session.settings().toolbar_iconsize, std::string("40"));
    ASSERT_EQ(boxes.texts().size(), 1);
    ASSERT_TRUE(boxes.texts().front().contains(configFile));
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
