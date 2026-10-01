#include "src/backend/config/menu_definition.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/ui/desktop/menu/menu_builder.h"
#include "src/ui/desktop/menu/testing/menu_snapshot.h"

#include <QApplication>
#include <QMenuBar>
#include <QObject>
#include <QToolBar>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace
{

// QMenuBar/QToolBar need a live QApplication; see menu_builder_test.cpp.
class LegacyMenuEnvironment final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        static int argc = 1;
        static auto program = std::to_array("legacy_menu_golden_test");
        static auto argv = std::to_array<char *>({program.data(), nullptr});
        app_ = std::make_unique<QApplication>(argc, argv.data());
    }

  private:
    std::unique_ptr<QApplication> app_;
};

const auto *legacy_menu_environment = ::testing::AddGlobalTestEnvironment(new LegacyMenuEnvironment);

std::string read_env_file(const char *variable)
{
    const char *path = std::getenv(variable);
    if (path == nullptr)
    {
        ADD_FAILURE() << variable << " must be set by the Bazel target's env";
        return {};
    }
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open()) << "cannot open " << path;
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

TEST(LegacyMenuGoldenTest, ShippedMenuCfgRendersAsTheGolden)
{
    const std::string cfg = read_env_file("MENU_CFG_PATH");
    fastecu::InMemoryFileRepository repository;
    repository.files["menu.cfg"] = std::vector<std::uint8_t>(cfg.begin(), cfg.end());
    fastecu::config::ConfigPaths paths;
    paths.menu_file = "menu.cfg";

    auto definition = fastecu::config::load_menu_definition(paths, repository);
    ASSERT_THAT(definition, fastecu::testing::IsOk());

    QMenuBar menubar;
    QToolBar toolbar;
    QObject parent;
    fastecu::ui::build_menus(*definition, &menubar, &toolbar, &parent);

    const std::string actual = fastecu::ui::testing::menu_snapshot(menubar, toolbar);
    EXPECT_EQ(actual, read_env_file("MENU_GOLDEN_PATH")) << "actual snapshot:\n" << actual;
}
