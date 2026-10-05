#include "src/ui/desktop/definition/dialog/definition_authoring_dialog.h"

#include <array>
#include <memory>

#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/definition/definition_catalog_session.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/ui/desktop/definition/definition_header_form.h"

using fastecu::ui::DefinitionAuthoringDialog;
using fastecu::ui::HeaderFormEditors;
using fastecu::ui::populate_header_dialog;
using ::testing::ElementsAre;

namespace
{

class AuthoringDialogEnvironment final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        static int argc = 1;
        static auto program = std::to_array("definition_authoring_dialog_test");
        static auto argv = std::to_array<char *>({program.data(), nullptr});
        app_ = std::make_unique<QApplication>(argc, argv.data());
    }

  private:
    std::unique_ptr<QApplication> app_;
};

const auto *authoring_dialog_environment = ::testing::AddGlobalTestEnvironment(new AuthoringDialogEnvironment);

} // namespace

// The object constructs with a composition-owned session and exposes the four
// log signals MainWindow connects -- a missing Q_OBJECT or a renamed signal
// links fine and fails only at runtime, which is exactly what this catches.
TEST(DefinitionAuthoringDialogTest, ConstructsAndExposesTheFourLogSignals)
{
    QWidget parent;
    fastecu::InMemoryFileRepository repository;
    QtFileSystem file_system;
    QtResourceBundle resource_bundle;
    QtFileRepository config_repository;
    QtAtomicFileWriter writer;
    fastecu::NullEventSink events;
    fastecu::config::testing::ConfigSessionFixture config;
    ASSERT_TRUE(config.initialize().has_value());
    fastecu::definition::DefinitionService service(file_system, config_repository, writer);
    fastecu::definition::DefinitionCatalogSession catalogs(service, config.session, file_system, events);

    DefinitionAuthoringDialog dialog(catalogs, config.session, repository, &parent);

    EXPECT_TRUE(fastecu::testing::SignalRecorder(&dialog, &DefinitionAuthoringDialog::LOG_E).is_valid());
    EXPECT_TRUE(fastecu::testing::SignalRecorder(&dialog, &DefinitionAuthoringDialog::LOG_W).is_valid());
    EXPECT_TRUE(fastecu::testing::SignalRecorder(&dialog, &DefinitionAuthoringDialog::LOG_I).is_valid());
    EXPECT_TRUE(fastecu::testing::SignalRecorder(&dialog, &DefinitionAuthoringDialog::LOG_D).is_valid());
}

// Regression test for the use-after-free this package shipped with: the form
// editors are children of the QDialog, so a dialog constructed inside the
// helper died on return and left create_new_definition reading freed
// QLineEdits. The contract the fix rests on is that the *caller* owns the
// dialog -- pin it here by keeping the dialog alive in this scope and
// reading the editors after the helper has returned.
TEST(DefinitionAuthoringDialogTest, HeaderEditorsStayReadableWhileTheCallerOwnsTheDialog)
{
    QDialog dialog;
    const QStringList labels{"XML ID", "ECU ID", "Notes"};
    const QStringList names{"xmlid", "ecuid", "notes"};
    const QStringList values{"3352a403", "39670016", "bench only"};

    const HeaderFormEditors editors = populate_header_dialog(dialog, labels, names, values);

    // Churn the Qt heap the way the real flow does (a QFileDialog is built
    // and destroyed between the header dialog closing and these reads), so a
    // regression reads recycled memory rather than a still-warm free block.
    for (int index = 0; index < 32; index++)
    {
        QWidget scratch;
        scratch.setObjectName("scratch");
    }

    ASSERT_EQ(editors.line_edits.size(), 2);
    ASSERT_EQ(editors.text_edits.size(), 1);
    EXPECT_EQ(editors.line_edits.at(0)->objectName(), QString("xmlid"));
    EXPECT_EQ(editors.line_edits.at(0)->text(), QString("3352a403"));
    EXPECT_EQ(editors.line_edits.at(1)->objectName(), QString("ecuid"));
    EXPECT_EQ(editors.line_edits.at(1)->text(), QString("39670016"));
    EXPECT_EQ(editors.text_edits.at(0)->objectName(), QString("notes"));
    EXPECT_EQ(editors.text_edits.at(0)->toPlainText(), QString("bench only"));

    // The coupling that makes the lifetime rule real: addLayout reparented
    // every editor onto the dialog, so the dialog's destructor owns them.
    EXPECT_EQ(editors.line_edits.at(0)->parentWidget(), &dialog);
    EXPECT_EQ(editors.text_edits.at(0)->parentWidget(), &dialog);

    // And the mapping the wizards perform on those editors still resolves.
    const auto input = fastecu::ui::definition_header_input(editors);
    ASSERT_TRUE(input.has_value());
    EXPECT_EQ(input->xml_id, "3352a403");
    EXPECT_EQ(input->ecu_id, "39670016");
    EXPECT_EQ(input->notes, "bench only");

    // The converse -- what the old code did by owning the dialog inside the
    // helper. A QPointer shows the editors dying with a dialog that goes out
    // of scope without having to read freed memory to prove it.
    QPointer<QLineEdit> tracked;
    {
        QDialog scoped_dialog;
        tracked = populate_header_dialog(scoped_dialog, labels, names, values).line_edits.at(0);
        ASSERT_FALSE(tracked.isNull());
    }
    EXPECT_TRUE(tracked.isNull());
}

TEST(DefinitionAuthoringDialog, FormInputRegistersTypedLookupAfterSuccessfulSubmission)
{
    QDialog dialog;
    const HeaderFormEditors editors = populate_header_dialog(
        dialog, {"XML ID", "Internal ID", "Internal ID address", "ECU ID"},
        {"xmlid", "internalidstring", "internalidaddress", "ecuid"}, {"3352a403", "CAL_ID", "7ffc", "39670016"});
    auto input = fastecu::ui::definition_header_input(editors);
    ASSERT_THAT(input, fastecu::testing::IsOk());
    fastecu::config::testing::ConfigSessionFixture config;
    ASSERT_THAT(config.initialize(), fastecu::testing::IsOk());
    fastecu::InMemoryAtomicFileWriter writer;
    fastecu::definition::DefinitionService service(config.file_system, config.file_repository, writer);
    fastecu::definition::DefinitionCatalogSession catalogs(service, config.session, config.file_system, config.events);
    ASSERT_THAT(catalogs.submit_new_definition("defs/colt.xml", *input, true), fastecu::testing::IsOk());
    EXPECT_EQ(catalogs.indexed_source(fastecu::definition::DefinitionFormat::EcuFlash, "3352a403"), "defs/colt.xml");
    writer.replace_error = {fastecu::ErrorKind::Disconnected, "unavailable"};
    input->xml_id = "FAILED";
    EXPECT_THAT(catalogs.submit_new_definition("defs/failed.xml", *input, true),
                fastecu::testing::IsErr(fastecu::ErrorKind::Disconnected));
    EXPECT_EQ(catalogs.indexed_source(fastecu::definition::DefinitionFormat::EcuFlash, "FAILED"), std::nullopt);
}

TEST(DefinitionHeaderFields, PinsAuthoredHeaderFields)
{
    EXPECT_EQ(fastecu::ui::definition_header_labels(),
              (QStringList{"XML ID", "Internal ID Address", "Internal ID String", "ECU ID", "Make", "Market", "Model",
                           "Submodel", "Transmission", "Year", "Flash Method", "Memory Model", "Checksum Module",
                           "Include", "Notes"}));
    EXPECT_EQ(
        fastecu::ui::definition_header_names(),
        (QStringList{"xmlid", "internalidaddress", "internalidstring", "ecuid", "make", "market", "model", "submodel",
                     "transmission", "year", "flashmethod", "memmodel", "checksummodule", "include", "notes"}));
}

namespace
{
class DefinitionAuthoringFlow : public testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(config.initialize(), fastecu::testing::IsOk());
        ASSERT_TRUE(root.isValid());
        config.session.settings().ecuflash_definition_files_directory = root.path().toStdString();
    }

    QTemporaryDir root;
    fastecu::config::testing::ConfigSessionFixture config;
    fastecu::InMemoryAtomicFileWriter writer;
    fastecu::definition::DefinitionService service{config.file_system, config.file_repository, writer};
    fastecu::definition::DefinitionCatalogSession catalogs{service, config.session, config.file_system, config.events};
    QWidget parent;
    DefinitionAuthoringDialog dialog{catalogs, config.session, config.file_repository, &parent};
};

TEST_F(DefinitionAuthoringFlow, CancelledCreateDoesNotWriteOrRegister)
{
    QTimer::singleShot(0, &dialog,
                       []
                       {
                           if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                           {
                               modal->reject();
                           }
                       });
    EXPECT_TRUE(dialog.create_new_definition());
    EXPECT_THAT(writer.replace_calls, testing::IsEmpty());
    EXPECT_EQ(catalogs.indexed_source(fastecu::definition::DefinitionFormat::EcuFlash, "NEW_XML"), std::nullopt);
}

TEST_F(DefinitionAuthoringFlow, CancelledImportAndRetryDoesNotWriteOrRegister)
{
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout,
                     []
                     {
                         if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                         {
                             modal->reject();
                         }
                     });
    driver.start(1);
    EXPECT_TRUE(dialog.use_existing_definition());
    EXPECT_THAT(writer.replace_calls, testing::IsEmpty());
    EXPECT_EQ(catalogs.indexed_source(fastecu::definition::DefinitionFormat::EcuFlash, "NEW_XML"), std::nullopt);
}

TEST_F(DefinitionAuthoringFlow, InvalidHeaderDoesNotWriteOrRegister)
{
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout,
                     [&]
                     {
                         if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()))
                         {
                             picker->selectFile(root.filePath("invalid.xml"));
                             QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                         }
                         else if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                         {
                             modal->accept();
                         }
                     });
    driver.start(1);
    EXPECT_FALSE(dialog.create_new_definition());
    EXPECT_THAT(writer.replace_calls, testing::IsEmpty());
    EXPECT_EQ(catalogs.indexed_source(fastecu::definition::DefinitionFormat::EcuFlash, ""), std::nullopt);
}

TEST_F(DefinitionAuthoringFlow, MalformedImportReportsErrorWithoutOpeningAnEditableHeader)
{
    const auto path = root.filePath("malformed.xml");
    QFile source(path);
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write("<rom><romid>");
    source.close();
    config.file_repository.files[path.toStdString()] = {'<', 'r', 'o', 'm', '>', '<', 'r', 'o', 'm', 'i', 'd', '>'};
    bool header_opened = false;
    bool error_shown = false;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout,
                     [&]
                     {
                         if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()))
                         {
                             picker->selectFile(path);
                             QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                         }
                         else if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                         {
                             error_shown = true;
                             message->accept();
                         }
                         else if (auto *header = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                         {
                             header_opened = true;
                             header->reject();
                         }
                     });
    driver.start(1);
    EXPECT_FALSE(dialog.use_existing_definition());
    EXPECT_TRUE(error_shown);
    EXPECT_FALSE(header_opened);
    EXPECT_THAT(writer.replace_calls, testing::IsEmpty());
}
} // namespace
