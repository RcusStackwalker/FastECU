#include "src/ui/desktop/definition/dialog/definition_authoring_dialog.h"

#include <array>
#include <functional>
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
#include "src/backend/ports/testing/null_event_sink.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/ui/desktop/definition/definition_header_form.h"

using fastecu::testing::IsOk;
using fastecu::ui::DefinitionAuthoringDialog;
using fastecu::ui::HeaderFormEditors;
using fastecu::ui::populateHeaderDialog;
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
    QtFileSystem fileSystem;
    QtResourceBundle resourceBundle;
    QtFileRepository configRepository;
    QtAtomicFileWriter writer;
    fastecu::NullEventSink events;
    fastecu::config::testing::ConfigSessionFixture config;
    ASSERT_THAT(config.Initialize(), IsOk());
    fastecu::definition::DefinitionService service(fileSystem, configRepository, writer);
    fastecu::definition::DefinitionCatalogSession catalogs(service, config.session, fileSystem, events);

    DefinitionAuthoringDialog dialog(catalogs, config.session, repository, &parent);

    EXPECT_TRUE(fastecu::testing::SignalRecorder(&dialog, &DefinitionAuthoringDialog::logE).IsValid());
    EXPECT_TRUE(fastecu::testing::SignalRecorder(&dialog, &DefinitionAuthoringDialog::logW).IsValid());
    EXPECT_TRUE(fastecu::testing::SignalRecorder(&dialog, &DefinitionAuthoringDialog::logI).IsValid());
    EXPECT_TRUE(fastecu::testing::SignalRecorder(&dialog, &DefinitionAuthoringDialog::logD).IsValid());
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

    const HeaderFormEditors editors = populateHeaderDialog(dialog, draft);

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
    const auto input = fastecu::ui::definitionHeaderInput(editors);
    ASSERT_THAT(input, IsOk());
    EXPECT_EQ(input->xml_id, "3352a403");
    EXPECT_EQ(input->ecu_id, "39670016");
    EXPECT_EQ(input->notes, "bench only");

    // The converse -- what the old code did by owning the dialog inside the
    // helper. A QPointer shows the editors dying with a dialog that goes out
    // of scope without having to read freed memory to prove it.
    QPointer<QLineEdit> tracked;
    {
        QDialog scopedDialog;
        tracked = populateHeaderDialog(scopedDialog, draft).xml_id;
        ASSERT_FALSE(tracked.isNull());
    }
    EXPECT_TRUE(tracked.isNull());
}

TEST(DefinitionAuthoringDialog, FormInputRegistersTypedLookupAfterSuccessfulSubmission)
{
    QDialog dialog;
    const HeaderFormEditors editors = populateHeaderDialog(
        dialog,
        {.xml_id = "3352a403", .internal_id = "CAL_ID", .ecu_id = "39670016", .internal_id_address_text = "7ffc"});
    auto input = fastecu::ui::definitionHeaderInput(editors);
    ASSERT_THAT(input, fastecu::testing::IsOk());
    fastecu::config::testing::ConfigSessionFixture config;
    ASSERT_THAT(config.Initialize(), fastecu::testing::IsOk());
    fastecu::InMemoryAtomicFileWriter writer;
    fastecu::definition::DefinitionService service(config.file_system, config.file_repository, writer);
    fastecu::definition::DefinitionCatalogSession catalogs(service, config.session, config.file_system, config.events);
    ASSERT_THAT(catalogs.SubmitNewDefinition("defs/colt.xml", *input, true), fastecu::testing::IsOk());
    EXPECT_EQ(catalogs.IndexedSource(fastecu::definition::DefinitionFormat::kEcuFlash, "3352a403"), "defs/colt.xml");
    writer.replace_error = {fastecu::ErrorKind::kDisconnected, "unavailable"};
    input->xml_id = "FAILED";
    EXPECT_THAT(catalogs.SubmitNewDefinition("defs/failed.xml", *input, true),
                fastecu::testing::IsErr(fastecu::ErrorKind::kDisconnected));
    EXPECT_EQ(catalogs.IndexedSource(fastecu::definition::DefinitionFormat::kEcuFlash, "FAILED"), std::nullopt);
}

namespace
{
class DefinitionAuthoringFlow : public testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(config_.Initialize(), fastecu::testing::IsOk());
        ASSERT_TRUE(root_.isValid());
        config_.session.Settings().ecuflash_definition_files_directory = root_.path().toStdString();
    }

    // Answers every modal a wizard opens until it returns: the open chooser
    // picks `source` and the save chooser `destination` (a chooser with no
    // path is rejected), a message box is acknowledged and counted, and any
    // other dialog -- the header form or the retry nag -- goes to `answer`.
    void drive(const QString& source, const QString& destination, std::function<void(QDialog&)> answer)
    {
        QObject::connect(&driver_, &QTimer::timeout,
                         [this, source, destination, answer = std::move(answer)]
                         {
                             auto *modal = QApplication::activeModalWidget();
                             if (auto *picker = qobject_cast<QFileDialog *>(modal))
                             {
                                 const auto path =
                                     picker->acceptMode() == QFileDialog::AcceptSave ? destination : source;
                                 if (path.isEmpty())
                                 {
                                     picker->reject();
                                     return;
                                 }
                                 picker->selectFile(path);
                                 QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                             }
                             else if (auto *message = qobject_cast<QMessageBox *>(modal))
                             {
                                 messages_++;
                                 message->accept();
                             }
                             else if (auto *dialog = qobject_cast<QDialog *>(modal))
                             {
                                 answer(*dialog);
                             }
                         });
        driver_.start(1);
    }

    QString writeSource(const QString& name, const std::vector<std::uint8_t>& bytes)
    {
        const auto path = root_.filePath(name);
        QFile source(path);
        EXPECT_TRUE(source.open(QIODevice::WriteOnly));
        source.write(reinterpret_cast<const char *>(bytes.data()), static_cast<qint64>(bytes.size()));
        config_.file_repository.files[path.toStdString()] = bytes;
        return path;
    }

    static void fillRequiredAndAccept(QDialog& header)
    {
        for (const char *name : {"xmlid", "internalidstring", "ecuid", "internalidaddress"})
        {
            if (auto *editor = header.findChild<QLineEdit *>(name); editor->text().isEmpty())
            {
                editor->setText("7ffc");
            }
        }
        header.accept();
    }

    QTemporaryDir root_;
    fastecu::config::testing::ConfigSessionFixture config_;
    fastecu::InMemoryAtomicFileWriter writer_;
    fastecu::definition::DefinitionService service_{config_.file_system, config_.file_repository, writer_};
    fastecu::definition::DefinitionCatalogSession catalogs_{service_, config_.session, config_.file_system,
                                                            config_.events};
    QWidget parent_;
    DefinitionAuthoringDialog dialog_{catalogs_, config_.session, config_.file_repository, &parent_};
    QTimer driver_;
    int messages_ = 0;
};

TEST_F(DefinitionAuthoringFlow, CompletedCreateWritesAndRegistersTheDestination)
{
    drive({}, root_.filePath("created"), fillRequiredAndAccept);
    EXPECT_TRUE(dialog_.createNewDefinition());
    ASSERT_THAT(writer_.replace_calls, testing::SizeIs(1));
    EXPECT_EQ(writer_.replace_calls[0].handle, root_.filePath("created.xml").toStdString());
    EXPECT_EQ(catalogs_.IndexedSource(fastecu::definition::DefinitionFormat::kEcuFlash, "7ffc"),
              root_.filePath("created.xml").toStdString());
}

TEST_F(DefinitionAuthoringFlow, CompletedImportWritesTheSourceHeaderToTheDestination)
{
    const std::string xml = "<rom><romid><xmlid>IMPORTED</xmlid></romid></rom>";
    const auto source = writeSource("source.xml", {xml.begin(), xml.end()});
    drive(source, root_.filePath("imported.xml"), fillRequiredAndAccept);
    EXPECT_TRUE(dialog_.useExistingDefinition());
    ASSERT_THAT(writer_.replace_calls, testing::SizeIs(1));
    EXPECT_EQ(writer_.replace_calls[0].handle, root_.filePath("imported.xml").toStdString());
    EXPECT_EQ(catalogs_.IndexedSource(fastecu::definition::DefinitionFormat::kEcuFlash, "IMPORTED"),
              root_.filePath("imported.xml").toStdString());
}

TEST_F(DefinitionAuthoringFlow, FailedWriteReportsFailure)
{
    writer_.replace_error = {fastecu::ErrorKind::kDisconnected, "unavailable"};
    drive({}, root_.filePath("created.xml"), fillRequiredAndAccept);
    EXPECT_FALSE(dialog_.createNewDefinition());
    EXPECT_THAT(writer_.replace_calls, testing::SizeIs(1));
    EXPECT_EQ(messages_, 1);
}

TEST_F(DefinitionAuthoringFlow, CancelledCreateDoesNotWriteOrRegister)
{
    drive({}, {}, [](QDialog& header) { header.reject(); });
    EXPECT_TRUE(dialog_.createNewDefinition());
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
}

TEST_F(DefinitionAuthoringFlow, CancelledImportAndRetryDoesNotWriteOrRegister)
{
    drive({}, {}, [](QDialog& nag) { nag.reject(); });
    EXPECT_TRUE(dialog_.useExistingDefinition());
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
}

TEST_F(DefinitionAuthoringFlow, InvalidHeaderDoesNotWriteOrRegister)
{
    drive({}, root_.filePath("invalid.xml"),
          [](QDialog& header)
          {
              header.findChild<QLineEdit *>("internalidaddress")->setText("not-an-address");
              header.accept();
          });
    const fastecu::testing::SignalRecorder errors(&dialog_, &DefinitionAuthoringDialog::logE);
    EXPECT_FALSE(dialog_.createNewDefinition());
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
    EXPECT_EQ(messages_, 1);
    ASSERT_EQ(errors.Count(), 1U);
    EXPECT_TRUE(std::get<0>(errors.Snapshot()[0]).startsWith("Invalid definition header: "));
}

TEST_F(DefinitionAuthoringFlow, MalformedImportReportsErrorWithoutOpeningAnEditableHeader)
{
    const auto path = writeSource("malformed.xml", {'<', 'r', 'o', 'm', '>', '<', 'r', 'o', 'm', 'i', 'd', '>'});
    bool headerOpened = false;
    drive(path, {},
          [&](QDialog& header)
          {
              headerOpened = true;
              header.reject();
          });
    EXPECT_FALSE(dialog_.useExistingDefinition());
    EXPECT_EQ(messages_, 1);
    EXPECT_FALSE(headerOpened);
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
    const auto path = writeSource("utf16.xml", bytes);
    QString importedId;
    drive(path, {},
          [&](QDialog& header)
          {
              importedId = header.findChild<QLineEdit *>("xmlid")->text();
              header.reject();
          });
    EXPECT_TRUE(dialog_.useExistingDefinition());
    EXPECT_EQ(messages_, 0);
    EXPECT_EQ(importedId, QString::fromUtf8("CAF\xc3\xa9"));
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
}
} // namespace
