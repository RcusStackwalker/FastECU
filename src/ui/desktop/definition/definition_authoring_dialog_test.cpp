#include "src/ui/desktop/definition/definition_authoring_dialog.h"

#include <memory>

#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QStringList>
#include <QWidget>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

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
using fastecu::ui::record_definition;
using ::testing::ElementsAre;

namespace
{

class AuthoringDialogEnvironment final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        static int argc = 1;
        static char program[] = "definition_authoring_dialog_test";
        static char *argv[] = {program, nullptr};
        app_ = std::make_unique<QApplication>(argc, argv);
    }

  private:
    std::unique_ptr<QApplication> app_;
};

const auto *authoring_dialog_environment = ::testing::AddGlobalTestEnvironment(new AuthoringDialogEnvironment);

} // namespace

// The two entry points are modal and are deliberately left untested, per the
// step-6a design: "the modal wiring itself is left untested, consistent with
// existing desktop UI practice." What is pinned here is that the object
// constructs without a live FileActions dialog parent and exposes the four
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
    FileActions file_actions(file_system, resource_bundle, config_repository, writer, events);

    DefinitionAuthoringDialog dialog(file_actions, repository, &parent);

    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_E).isValid());
    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_W).isValid());
    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_I).isValid());
    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_D).isValid());
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

// The four ecuflash_def_* config lists are appended to only once the
// definition has been written; both wizards funnel that through
// record_definition, which sits after the submit_*_definition status check.
// This pins which form field lands in which list and that repeated records
// append index-aligned entries rather than overwriting.
TEST(DefinitionAuthoringDialogTest, RecordDefinitionAppendsTheFourConfigListsInStep)
{
    QDialog dialog;
    const QStringList labels{"XML ID", "Internal ID address", "ECU ID"};
    const QStringList names{"xmlid", "internalidaddress", "ecuid"};
    const HeaderFormEditors editors =
        populate_header_dialog(dialog, labels, names, QStringList{"3352a403", "7ffc", "39670016"});
    const auto input = fastecu::ui::definition_header_input(editors);
    ASSERT_TRUE(input.has_value());

    FileActions::ConfigValuesStructure config;
    record_definition(config, editors, *input, "defs/colt.xml");

    EXPECT_THAT(config.ecuflash_def_cal_id, ElementsAre(QString("3352a403")));
    EXPECT_THAT(config.ecuflash_def_cal_id_addr, ElementsAre(QString("7ffc")));
    EXPECT_THAT(config.ecuflash_def_ecu_id, ElementsAre(QString("39670016")));
    EXPECT_THAT(config.ecuflash_def_filename, ElementsAre(QString("defs/colt.xml")));

    QDialog second_dialog;
    const HeaderFormEditors second_editors =
        populate_header_dialog(second_dialog, labels, names, QStringList{"39670016", "7ff8", "39670017"});
    const auto second_input = fastecu::ui::definition_header_input(second_editors);
    ASSERT_TRUE(second_input.has_value());

    record_definition(config, second_editors, *second_input, "defs/z27a.xml");

    EXPECT_THAT(config.ecuflash_def_cal_id, ElementsAre(QString("3352a403"), QString("39670016")));
    EXPECT_THAT(config.ecuflash_def_cal_id_addr, ElementsAre(QString("7ffc"), QString("7ff8")));
    EXPECT_THAT(config.ecuflash_def_ecu_id, ElementsAre(QString("39670016"), QString("39670017")));
    EXPECT_THAT(config.ecuflash_def_filename, ElementsAre(QString("defs/colt.xml"), QString("defs/z27a.xml")));
}
