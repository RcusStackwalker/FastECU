#include "src/ui/desktop/definition/definition_authoring_dialog.h"

#include <memory>

#include <QApplication>
#include <QSignalSpy>
#include <QWidget>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"

using fastecu::ui::DefinitionAuthoringDialog;

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
    FileActions file_actions(file_system, resource_bundle, config_repository, writer);

    DefinitionAuthoringDialog dialog(file_actions, repository, &parent);

    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_E).isValid());
    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_W).isValid());
    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_I).isValid());
    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_D).isValid());
}
