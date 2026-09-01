#include "src/backend/config/menu_definition.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using fastecu::ErrorKind;
using fastecu::InMemoryFileRepository;
using fastecu::config::ConfigPaths;
using fastecu::config::load_menu_definition;
using fastecu::config::MenuDefinition;

namespace
{
ConfigPaths test_paths()
{
    ConfigPaths paths;
    paths.menu_file = "menu.cfg";
    return paths;
}

void give(InMemoryFileRepository& repository, std::string_view xml)
{
    repository.files["menu.cfg"] = std::vector<std::uint8_t>(xml.begin(), xml.end());
}

constexpr std::string_view kTwoItems = R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
    <ecu_menu_definitions>
        <menu name="File">
            <menuitem name="Open calibration" id="open_calibration" checkable="false" shortcut="Ctrl+O" toolbar="true" icon=":/icons/document-open.png" tooltip="Open calibration file." />
            <menuitem name="Separator" id="separator" checkable="" shortcut="" toolbar="" icon="" tooltip="" />
        </menu>
    </ecu_menu_definitions>
</config>)";
} // namespace

TEST(MenuDefinitionTest, ParsesMenuNameAndItemAttributes)
{
    InMemoryFileRepository repository;
    give(repository, kTwoItems);

    auto definition = load_menu_definition(test_paths(), repository);

    ASSERT_TRUE(definition.has_value());
    ASSERT_EQ(definition->size(), 1U);
    EXPECT_EQ((*definition)[0].name, "File");
    ASSERT_EQ((*definition)[0].entries.size(), 2U);

    const auto& open = (*definition)[0].entries[0];
    EXPECT_FALSE(open.is_submenu);
    EXPECT_EQ(open.item.name, "Open calibration");
    EXPECT_EQ(open.item.id, "open_calibration");
    EXPECT_EQ(open.item.shortcut, "Ctrl+O");
    EXPECT_EQ(open.item.icon, ":/icons/document-open.png");
    EXPECT_EQ(open.item.tooltip, "Open calibration file.");
    EXPECT_FALSE(open.item.is_checkable());
    EXPECT_TRUE(open.item.on_toolbar());
    EXPECT_FALSE(open.item.is_separator());

    EXPECT_TRUE((*definition)[0].entries[1].item.is_separator());
}

TEST(MenuDefinitionTest, AbsentAttributesGetLegacySentinelDefaults)
{
    InMemoryFileRepository repository;
    give(repository, R"(<config><ecu_menu_definitions><menu>
        <menuitem />
    </menu></ecu_menu_definitions></config>)");

    auto definition = load_menu_definition(test_paths(), repository);

    ASSERT_TRUE(definition.has_value());
    ASSERT_EQ(definition->size(), 1U);
    EXPECT_EQ((*definition)[0].name, "No name");
    ASSERT_EQ((*definition)[0].entries.size(), 1U);
    const auto& item = (*definition)[0].entries[0].item;
    EXPECT_EQ(item.name, "No name");
    EXPECT_EQ(item.id, "No id");
    EXPECT_EQ(item.checkable, "No checkable");
    EXPECT_EQ(item.shortcut, "No shortcut");
    EXPECT_EQ(item.toolbar, "No toolbar");
    EXPECT_EQ(item.icon, "No icon");
    EXPECT_EQ(item.tooltip, "No tooltip");
    // "No checkable" is not the literal "true", so the predicate is false.
    EXPECT_FALSE(item.is_checkable());
    EXPECT_FALSE(item.on_toolbar());
}

TEST(MenuDefinitionTest, OnlyLiteralTrueEnablesCheckableAndToolbar)
{
    InMemoryFileRepository repository;
    give(repository, R"(<config><ecu_menu_definitions><menu name="Ecu">
        <menuitem name="Logging" id="toggle_realtime" checkable="true" toolbar="true" />
        <menuitem name="Other" id="other" checkable="TRUE" toolbar="1" />
    </menu></ecu_menu_definitions></config>)");

    auto definition = load_menu_definition(test_paths(), repository);

    ASSERT_TRUE(definition.has_value());
    ASSERT_EQ((*definition)[0].entries.size(), 2U);
    EXPECT_TRUE((*definition)[0].entries[0].item.is_checkable());
    EXPECT_TRUE((*definition)[0].entries[0].item.on_toolbar());
    EXPECT_FALSE((*definition)[0].entries[1].item.is_checkable());
    EXPECT_FALSE((*definition)[0].entries[1].item.on_toolbar());
}

TEST(MenuDefinitionTest, ParsesNestedSubmenuInDocumentOrder)
{
    InMemoryFileRepository repository;
    give(repository, R"(<config><ecu_menu_definitions><menu name="Top">
        <menuitem name="Before" id="before" />
        <menu name="Sub">
            <menuitem name="Inner" id="inner" toolbar="true" />
        </menu>
        <menuitem name="After" id="after" />
    </menu></ecu_menu_definitions></config>)");

    auto definition = load_menu_definition(test_paths(), repository);

    ASSERT_TRUE(definition.has_value());
    ASSERT_EQ((*definition)[0].entries.size(), 3U);
    EXPECT_FALSE((*definition)[0].entries[0].is_submenu);
    EXPECT_EQ((*definition)[0].entries[0].item.id, "before");

    const auto& sub = (*definition)[0].entries[1];
    ASSERT_TRUE(sub.is_submenu);
    EXPECT_EQ(sub.submenu_name, "Sub");
    ASSERT_EQ(sub.submenu_items.size(), 1U);
    EXPECT_EQ(sub.submenu_items[0].id, "inner");
    EXPECT_TRUE(sub.submenu_items[0].on_toolbar());

    EXPECT_FALSE((*definition)[0].entries[2].is_submenu);
    EXPECT_EQ((*definition)[0].entries[2].item.id, "after");
}

TEST(MenuDefinitionTest, PropagatesRepositoryReadFailure)
{
    InMemoryFileRepository repository; // no "menu.cfg" stored

    auto definition = load_menu_definition(test_paths(), repository);

    ASSERT_FALSE(definition.has_value());
    EXPECT_EQ(definition.error().kind, ErrorKind::InvalidConfig);
}

TEST(MenuDefinitionTest, MalformedXmlIsInvalidConfig)
{
    InMemoryFileRepository repository;
    give(repository, "<config><ecu_menu_definitions><menu name=\"Broken\">");

    auto definition = load_menu_definition(test_paths(), repository);

    ASSERT_FALSE(definition.has_value());
    EXPECT_EQ(definition.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(definition.error().detail, testing::HasSubstr("menu parse error"));
}
