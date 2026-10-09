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

const auto *const kAuthoringDialogEnvironment = ::testing::AddGlobalTestEnvironment(new AuthoringDialogEnvironment);

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
    const fastecu::definition::DefinitionHeaderDraft draft{
        .xml_id = "3352a403", .ecu_id = "39670016", .notes = "bench only"};

    const HeaderFormEditors editors = populate_header_dialog(dialog, draft);

    // Churn the Qt heap the way the real flow does (a QFileDialog is built
    // and destroyed between the header dialog closing and these reads), so a
    // regression reads recycled memory rather than a still-warm free block.
    for (int index = 0; index < 32; index++)
    {
        QWidget scratch;
        scratch.setObjectName("scratch");
    }

    EXPECT_EQ(editors.xml_id->objectName(), QString("xmlid"));
    EXPECT_EQ(editors.xml_id->text(), QString("3352a403"));
    EXPECT_EQ(editors.ecu_id->objectName(), QString("ecuid"));
    EXPECT_EQ(editors.ecu_id->text(), QString("39670016"));
    EXPECT_EQ(editors.notes->objectName(), QString("notes"));
    EXPECT_EQ(editors.notes->toPlainText(), QString("bench only"));

    // The coupling that makes the lifetime rule real: addLayout reparented
    // every editor onto the dialog, so the dialog's destructor owns them.
    EXPECT_EQ(editors.xml_id->parentWidget(), &dialog);
    EXPECT_EQ(editors.notes->parentWidget(), &dialog);

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
        tracked = populate_header_dialog(scoped_dialog, draft).xml_id;
        ASSERT_FALSE(tracked.isNull());
    }
    EXPECT_TRUE(tracked.isNull());
}

TEST(DefinitionAuthoringDialog, FormInputRegistersTypedLookupAfterSuccessfulSubmission)
{
    QDialog dialog;
    const HeaderFormEditors editors = populate_header_dialog(
        dialog,
        {.xml_id = "3352a403", .internal_id = "CAL_ID", .ecu_id = "39670016", .internal_id_address_text = "7ffc"});
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

namespace
{
class DefinitionAuthoringFlow : public testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(config_.initialize(), fastecu::testing::IsOk());
        ASSERT_TRUE(root_.isValid());
        config_.session.settings().ecuflash_definition_files_directory = root_.path().toStdString();
    }

    QTemporaryDir root_;
    fastecu::config::testing::ConfigSessionFixture config_;
    fastecu::InMemoryAtomicFileWriter writer_;
    fastecu::definition::DefinitionService service_{config_.file_system, config_.file_repository, writer_};
    fastecu::definition::DefinitionCatalogSession catalogs_{service_, config_.session, config_.file_system,
                                                            config_.events};
    QWidget parent_;
    DefinitionAuthoringDialog dialog_{catalogs_, config_.session, config_.file_repository, &parent_};
};

TEST_F(DefinitionAuthoringFlow, CancelledCreateDoesNotWriteOrRegister)
{
    QTimer::singleShot(0, &dialog_,
                       []
                       {
                           if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                           {
                               modal->reject();
                           }
                       });
    EXPECT_TRUE(dialog_.create_new_definition());
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
    EXPECT_EQ(catalogs_.indexed_source(fastecu::definition::DefinitionFormat::EcuFlash, "NEW_XML"), std::nullopt);
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
    EXPECT_TRUE(dialog_.use_existing_definition());
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
    EXPECT_EQ(catalogs_.indexed_source(fastecu::definition::DefinitionFormat::EcuFlash, "NEW_XML"), std::nullopt);
}

TEST_F(DefinitionAuthoringFlow, InvalidHeaderDoesNotWriteOrRegister)
{
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout,
                     [&]
                     {
                         if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()))
                         {
                             picker->selectFile(root_.filePath("invalid.xml"));
                             QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                         }
                         else if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                         {
                             modal->accept();
                         }
                     });
    driver.start(1);
    EXPECT_FALSE(dialog_.create_new_definition());
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
    EXPECT_EQ(catalogs_.indexed_source(fastecu::definition::DefinitionFormat::EcuFlash, ""), std::nullopt);
}

TEST_F(DefinitionAuthoringFlow, MalformedImportReportsErrorWithoutOpeningAnEditableHeader)
{
    const auto path = root_.filePath("malformed.xml");
    QFile source(path);
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write("<rom><romid>");
    source.close();
    config_.file_repository.files[path.toStdString()] = {'<', 'r', 'o', 'm', '>', '<', 'r', 'o', 'm', 'i', 'd', '>'};
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
    EXPECT_FALSE(dialog_.use_existing_definition());
    EXPECT_TRUE(error_shown);
    EXPECT_FALSE(header_opened);
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
}

TEST_F(DefinitionAuthoringFlow, Utf16ImportOpensDecodedHeaderBeforeAnyWrite)
{
    const std::u16string xml = u"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
                               "<rom><romid><xmlid>CAF\u00e9</xmlid></romid></rom>";
    std::vector<std::uint8_t> bytes{0xff, 0xfe};
    for (const char16_t value : xml)
    {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    }
    const auto path = root_.filePath("utf16.xml");
    QFile source(path);
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write(reinterpret_cast<const char *>(bytes.data()), static_cast<qint64>(bytes.size()));
    source.close();
    config_.file_repository.files[path.toStdString()] = bytes;
    bool header_opened = false;
    bool error_shown = false;
    QString imported_id;
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
                             const auto *editor = header->findChild<QLineEdit *>("xmlid");
                             if (editor)
                             {
                                 imported_id = editor->text();
                             }
                             header->reject();
                         }
                     });
    driver.start(1);
    EXPECT_TRUE(dialog_.use_existing_definition());
    EXPECT_TRUE(header_opened);
    EXPECT_FALSE(error_shown);
    EXPECT_EQ(imported_id, QString::fromUtf8("CAF\xc3\xa9"));
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
}
} // namespace
