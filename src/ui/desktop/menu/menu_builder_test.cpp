#include "src/ui/desktop/menu/menu_builder.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMenuBar>
#include <QObject>
#include <QToolBar>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

using fastecu::config::Menu;
using fastecu::config::MenuDefinition;
using fastecu::config::MenuEntry;
using fastecu::config::MenuItem;

namespace
{

// QMenuBar/QToolBar are QWidgets, which abort at construction without a live
// QApplication. This suite links fastecu_gtest's plain gtest_main (it
// declares no Q_OBJECT, so fastecu_qttest's QTEST_MAIN generator doesn't
// apply), so bring one up via a ::testing::Environment, mirroring
// QtPortEnvironment in
// src/platform/desktop/common/ports/qt_port_adapters_test.cpp. SetUp() runs
// after static initialization and after InitGoogleTest, and gtest tears the
// Environment down deterministically after all tests -- both properties the
// alternative of a file-scope static QApplication lacks.
class MenuBuilderEnvironment final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        static int argc = 1;
        static char program[] = "menu_builder_test";
        static char *argv[] = {program, nullptr};
        app_ = std::make_unique<QApplication>(argc, argv);
    }

  private:
    std::unique_ptr<QApplication> app_;
};

const auto *menu_builder_environment = ::testing::AddGlobalTestEnvironment(new MenuBuilderEnvironment);

MenuEntry plain(const MenuItem& item)
{
    MenuEntry entry;
    entry.item = item;
    return entry;
}

MenuItem named(std::string name, std::string id)
{
    MenuItem item;
    item.name = std::move(name);
    item.id = std::move(id);
    return item;
}
} // namespace

TEST(MenuBuilderTest, BuildsOneMenuPerDefinitionEntryWithItsActions)
{
    QMenuBar menubar;
    QToolBar toolBar;
    QObject parent;

    Menu file;
    file.name = "File";
    file.entries = {plain(named("Open calibration", "open_calibration")), plain(named("Separator", "separator")),
                    plain(named("Quit", "quit"))};

    QSignalMapper *mapper = build_menus(MenuDefinition{file}, &menubar, &toolBar, &parent);

    ASSERT_NE(mapper, nullptr);
    EXPECT_EQ(mapper->parent(), &parent);
    ASSERT_EQ(menubar.actions().size(), 1);
    QMenu *built = menubar.actions().at(0)->menu();
    ASSERT_NE(built, nullptr);
    EXPECT_EQ(built->title(), "File");

    ASSERT_EQ(built->actions().size(), 3);
    EXPECT_EQ(built->actions().at(0)->objectName(), "open_calibration");
    EXPECT_TRUE(built->actions().at(1)->isSeparator());
    EXPECT_EQ(built->actions().at(2)->objectName(), "quit");
}

TEST(MenuBuilderTest, AddsOnlyToolbarTrueItemsAndOneSeparatorPerContributingMenu)
{
    QMenuBar menubar;
    QToolBar toolBar;
    QObject parent;

    MenuItem on_bar = named("On bar", "on_bar");
    on_bar.toolbar = "true";
    MenuItem off_bar = named("Off bar", "off_bar");
    off_bar.toolbar = "false";

    Menu with_icon;
    with_icon.name = "First";
    with_icon.entries = {plain(on_bar), plain(off_bar)};

    Menu without_icon;
    without_icon.name = "Second";
    without_icon.entries = {plain(off_bar)};

    build_menus(MenuDefinition{with_icon, without_icon}, &menubar, &toolBar, &parent);

    // "on_bar", then one separator closing the menu that contributed it.
    // The second menu contributes nothing, so it adds no separator.
    ASSERT_EQ(toolBar.actions().size(), 2);
    EXPECT_EQ(toolBar.actions().at(0)->objectName(), "on_bar");
    EXPECT_TRUE(toolBar.actions().at(1)->isSeparator());
}

TEST(MenuBuilderTest, TooltipPrefixIsTheSubmenuNameForSubmenuItemsAndTheOwnNameForTopLevel)
{
    QMenuBar menubar;
    QToolBar toolBar;
    QObject parent;

    MenuItem top = named("Top item", "top_item");
    top.tooltip = "Top tip";
    MenuItem inner = named("Inner item", "inner_item");
    inner.tooltip = "Inner tip";

    MenuEntry submenu;
    submenu.is_submenu = true;
    submenu.submenu_name = "Sub";
    submenu.submenu_items = {inner};

    Menu menu;
    menu.name = "Top";
    menu.entries = {plain(top), submenu};

    build_menus(MenuDefinition{menu}, &menubar, &toolBar, &parent);

    QMenu *built = menubar.actions().at(0)->menu();
    ASSERT_EQ(built->actions().size(), 2);
    EXPECT_EQ(built->actions().at(0)->toolTip(), QString("Top item\n\nTop tip"));

    QMenu *sub = built->actions().at(1)->menu();
    ASSERT_NE(sub, nullptr);
    ASSERT_EQ(sub->actions().size(), 1);
    EXPECT_EQ(sub->actions().at(0)->toolTip(), QString("Sub\n\nInner tip"));
}

TEST(MenuBuilderTest, AppliesCheckableAndShortcutFromTheModel)
{
    QMenuBar menubar;
    QToolBar toolBar;
    QObject parent;

    MenuItem toggle = named("Logging", "toggle_realtime");
    toggle.checkable = "true";
    toggle.shortcut = "Space";
    MenuItem plainer = named("Quit", "quit");
    plainer.checkable = "No checkable";

    Menu menu;
    menu.name = "Ecu";
    menu.entries = {plain(toggle), plain(plainer)};

    build_menus(MenuDefinition{menu}, &menubar, &toolBar, &parent);

    QMenu *built = menubar.actions().at(0)->menu();
    EXPECT_TRUE(built->actions().at(0)->isCheckable());
    EXPECT_EQ(built->actions().at(0)->shortcut(), QKeySequence("Space"));
    EXPECT_FALSE(built->actions().at(1)->isCheckable());
}

TEST(MenuBuilderTest, TriggeringAnActionMapsToItsId)
{
    QMenuBar menubar;
    QToolBar toolBar;
    QObject parent;

    Menu menu;
    menu.name = "File";
    menu.entries = {plain(named("Quit", "quit"))};

    QSignalMapper *mapper = build_menus(MenuDefinition{menu}, &menubar, &toolBar, &parent);

    QString mapped;
    QObject::connect(mapper, &QSignalMapper::mappedString, [&mapped](const QString& id) { mapped = id; });
    menubar.actions().at(0)->menu()->actions().at(0)->trigger();

    EXPECT_EQ(mapped, QString("quit"));
}

TEST(MenuBuilderTest, EmptyDefinitionBuildsNothingButStillReturnsAMapper)
{
    QMenuBar menubar;
    QToolBar toolBar;
    QObject parent;

    QSignalMapper *mapper = build_menus(MenuDefinition{}, &menubar, &toolBar, &parent);

    ASSERT_NE(mapper, nullptr);
    EXPECT_TRUE(menubar.actions().isEmpty());
    EXPECT_TRUE(toolBar.actions().isEmpty());
}
