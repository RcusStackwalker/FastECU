# Step 6a-1 Menu Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split `FileActions::read_menu_file` into a portable pugixml parser in `src/backend/config` and a Qt widget builder in `src/ui/desktop/menu`, removing 210 lines of UI construction from the backend and putting the menu grammar under test for the first time.

**Architecture:** The legacy function parses `menu.cfg` with `QDomDocument` and builds `QMenu`/`QAction`/`QToolBar`/`QSignalMapper` in the same loop. The seam runs between those two halves. `load_menu_definition(const ConfigPaths&, IFileRepository&) -> Result<MenuDefinition>` mirrors the existing `load_protocol_catalog` exactly — same package, same signature shape, same `IFileRepository` byte source, same `ErrorKind::InvalidConfig` on parse failure. `build_menus(const MenuDefinition&, QMenuBar*, QToolBar*, QObject*) -> QSignalMapper*` owns every widget concern, including the legacy tooltip-prefix asymmetry. `MainWindow` calls one then the other.

**Tech Stack:** C++23, Bazel 9.1.1, pugixml, GoogleTest/GoogleMock 1.17.0.bcr.2, Qt 6.8.3 (Widgets).

**Spec:** [docs/superpowers/specs/2026-09-01-step6a-file-actions-dewidget-design.md](../specs/2026-09-01-step6a-file-actions-dewidget-design.md)

## Global Constraints

- **Do not edit any file under `src/platform/desktop/common/flash/legacy/` or `src/ui/desktop/flash/`.** The flash drain owns those paths on a parallel branch. `git diff --name-only origin/master` must contain none of them.
- **Out of scope for this PR** (later 6a slices): dropping `QWidget`/`Q_OBJECT` from `FileActions`, the `QMessageBox` → `IEventSink` conversion, the definition-authoring dialogs, and the `//src/algorithms/expression:qt_compat` drain. `FileActions` stays a `QWidget` at the end of this plan.
- Every header needs `#pragma once` (enforced by prek).
- Portable targets under `src/backend/**` must not reach Qt or JNI. Registration is **dual**: the `genquery` in the root `BUILD.bazel` and `PORTABLE_ROOTS` in `scripts/check-portable-closure.py`. Adding one without the other fails `//:portable_closure`.
- Backend operations return `fastecu::Result<T>`, checked with `.has_value()`, never the implicit `operator bool`.
- Prefer `std::string_view` by value, `std::format` for message construction, and ranges/views over index loops ([coding style guide](../../coding-style.md)).
- Run `prek run --all-files` before each commit; it runs clang-format, buildifier, and the pragma-once check.
- **Deviation from the spec's §Testing, deliberate:** the builder test uses `fastecu_gtest` (GoogleTest + `QT_DEPS`), not `fastecu_qttest`. `fastecu_qttest` generates moc for a self-including source, which is only needed when the test itself declares `Q_OBJECT`. This one does not. `src/ui/desktop/checksum/checksum_correction_command_test.cpp` is the exact precedent: Qt widgets, `QT_QPA_PLATFORM=offscreen`, `fastecu_gtest`.

## Behavior To Preserve

Transcribed from `src/backend/definitions/file_actions.cpp:796-1005`. These are the details a reimplementation silently gets wrong:

1. **Sentinel attribute defaults, not empty strings.** `QDomElement::attribute(name, default)` is called with `"No name"`, `"No id"`, `"No checkable"`, `"No shortcut"`, `"No toolbar"`, `"No icon"`, `"No tooltip"`. An absent attribute yields that literal.
2. **Only the literal `"true"` enables.** `checkable == "true"` and `toolbar == "true"`; everything else, including `"No checkable"`, is false.
3. **`name == "Separator"` means a separator** and no action is created.
4. **Tooltip prefix is asymmetric.** A *submenu's* items get `submenu_name + "\n\n" + tooltip`. A *top-level* item gets `its own name + "\n\n" + tooltip`.
5. **One toolbar separator per top-level menu that contributed any icon**, added after that menu is finished; the flag resets per top-level menu and is shared with its submenus' items.
6. **`QIcon` is constructed unconditionally**, even from `"No icon"` (a null icon), and `setIconVisibleInMenu(true)` is always called.
7. **The grammar is exactly two levels deep.** A top-level `<menu>`'s children are `<menu>` or `<menuitem>`; a submenu's children are `<menuitem>` only. The model is a flat pair, not a recursive node.
8. **`<popup_menu_definitions>` is ignored.** Its handler is commented out at `file_actions.cpp:966-999`, so parsing it would add menus that do not exist today.
9. **A missing `<config>` or `<ecu_menu_definitions>` is not an error.** Legacy simply builds nothing and returns an empty mapper.
10. **Comment handling diverges, deliberately and invisibly.** Legacy walks siblings with `nextSibling().toElement()`, which returns a null element for an XML comment and *terminates the loop*. pugixml skips comments and continues. In the shipped `menu.cfg` the only comment block is the last thing inside `<menu name="View">`, so both behaviors produce identical menus. Task 2 pins this with an explicit test rather than leaving it to be rediscovered.

## File Structure

| File | Responsibility |
|---|---|
| `src/backend/config/menu_definition.h` (create) | `MenuItem` / `MenuEntry` / `Menu` / `MenuDefinition` model, `load_menu_definition` declaration |
| `src/backend/config/menu_definition.cpp` (create) | pugixml parse of the `<ecu_menu_definitions>` section |
| `src/backend/config/menu_definition_test.cpp` (create) | Portable parser tests: synthetic grammar cases, then the shipped `menu.cfg` golden |
| `src/backend/config/BUILD.bazel` (modify) | `cc_library(menu_definition)` + `fastecu_portable_gtest(menu_definition_test)` |
| `resources/shared/BUILD.bazel` (modify) | Export `config/menu.cfg` to `//src/backend/config` |
| `BUILD.bazel` (modify, root) | Register `//src/backend/config:menu_definition` in the portable `genquery` |
| `scripts/check-portable-closure.py` (modify) | Register `menu_definition` in `PORTABLE_ROOTS` |
| `src/ui/desktop/menu/menu_builder.h` (create) | `build_menus` declaration |
| `src/ui/desktop/menu/menu_builder.cpp` (create) | Widget construction, tooltip prefixes, toolbar separators, signal mapping |
| `src/ui/desktop/menu/menu_builder_test.cpp` (create) | Offscreen assertions on the built menu/toolbar/mapper |
| `src/ui/desktop/menu/BUILD.bazel` (create) | `qt_cc_library(menu_builder)` + `fastecu_gtest(menu_builder_test)` |
| `src/ui/desktop/mainwindow.cpp:200` (modify) | Call `load_menu_definition` then `build_menus` |
| `src/ui/desktop/BUILD.bazel` (modify) | Depend on `//src/ui/desktop/menu:menu_builder` and `//src/backend/config:menu_definition` |
| `src/backend/definitions/file_actions.h` (modify) | Delete the `read_menu_file` declaration |
| `src/backend/definitions/file_actions.cpp:796-1005` (modify) | Delete the definition |

---

### Task 1: Portable `MenuDefinition` model and parser

**Files:**
- Create: `src/backend/config/menu_definition.h`
- Create: `src/backend/config/menu_definition.cpp`
- Test: `src/backend/config/menu_definition_test.cpp`
- Modify: `src/backend/config/BUILD.bazel`
- Modify: `BUILD.bazel` (root, the portable `genquery` target list near line 43)
- Modify: `scripts/check-portable-closure.py` (the `src/backend/config` entry, near line 99)

**Interfaces:**
- Consumes: `fastecu::config::ConfigPaths` (has a `menu_file` member already), `fastecu::IFileRepository::read(std::string_view) -> Result<std::vector<std::uint8_t>>`, `fastecu::fail(ErrorKind, std::string)`, `fastecu::InMemoryFileRepository` (test double, has a `files` map keyed by handle).
- Produces: `fastecu::config::MenuItem` with fields `name`, `id`, `checkable`, `shortcut`, `toolbar`, `icon`, `tooltip` (all `std::string`) and predicates `is_separator()`, `is_checkable()`, `on_toolbar()`; `fastecu::config::MenuEntry` with `is_submenu` (`bool`), `submenu_name` (`std::string`), `submenu_items` (`std::vector<MenuItem>`), `item` (`MenuItem`); `fastecu::config::Menu` with `name` and `entries` (`std::vector<MenuEntry>`); `using MenuDefinition = std::vector<Menu>`; and `Result<MenuDefinition> load_menu_definition(const ConfigPaths&, IFileRepository&)`. Task 3 builds widgets from these; Task 4 calls the function.

- [ ] **Step 1: Write the failing test**

Create `src/backend/config/menu_definition_test.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/config:menu_definition_test`
Expected: FAIL — the target does not exist yet (`no such target '//src/backend/config:menu_definition_test'`).

- [ ] **Step 3: Write the model header**

Create `src/backend/config/menu_definition.h`:

```cpp
#pragma once
#include <string>
#include <vector>

#include "src/backend/config/config_paths.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::config
{

// One <menuitem> element from menu.cfg. Every field keeps the sentinel
// default the legacy QDomElement::attribute(name, default) calls used
// (file_actions.cpp:796-1005) rather than an empty string: the builder
// branches on the literal "true", and shipped entries do omit attributes.
struct MenuItem
{
    std::string name{"No name"};
    std::string id{"No id"};
    std::string checkable{"No checkable"};
    std::string shortcut{"No shortcut"};
    std::string toolbar{"No toolbar"};
    std::string icon{"No icon"};
    std::string tooltip{"No tooltip"};

    // Legacy tests the literal string in all three cases; anything else,
    // including the "No ..." sentinels, is false.
    bool is_separator() const
    {
        return name == "Separator";
    }
    bool is_checkable() const
    {
        return checkable == "true";
    }
    bool on_toolbar() const
    {
        return toolbar == "true";
    }

    bool operator==(const MenuItem&) const = default;
};

// One ordered child of a top-level <menu>: either a <menuitem> or a nested
// <menu>. The legacy grammar is exactly two levels deep -- a submenu's
// children are only <menuitem> -- so this is a flat pair rather than a
// recursive node. Order matters: a submenu and the items around it appear in
// the menu in document order.
struct MenuEntry
{
    bool is_submenu{false};
    std::string submenu_name;            // set when is_submenu
    std::vector<MenuItem> submenu_items; // set when is_submenu
    MenuItem item;                       // set when !is_submenu

    bool operator==(const MenuEntry&) const = default;
};

struct Menu
{
    std::string name{"No name"};
    std::vector<MenuEntry> entries;

    bool operator==(const Menu&) const = default;
};

using MenuDefinition = std::vector<Menu>;

// Replaces the parsing half of FileActions::read_menu_file. Reads
// paths.menu_file through the repository and returns the
// <ecu_menu_definitions> section.
//
// <popup_menu_definitions> is deliberately ignored: its legacy handler is
// commented out (file_actions.cpp:966-999), so parsing it would add menus
// that do not exist today. A file with no <config> or no
// <ecu_menu_definitions> yields an empty MenuDefinition rather than an
// error -- legacy built no menus in that case and reported nothing.
Result<MenuDefinition> load_menu_definition(const ConfigPaths& paths, IFileRepository& file_repository);

} // namespace fastecu::config
```

- [ ] **Step 4: Write the parser**

Create `src/backend/config/menu_definition.cpp`:

```cpp
#include "src/backend/config/menu_definition.h"

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#include <pugixml.hpp>

namespace fastecu::config
{
namespace
{

std::string attribute_or(pugi::xml_node node, const char *name, const char *fallback)
{
    pugi::xml_attribute attribute = node.attribute(name);
    return attribute ? std::string(attribute.value()) : std::string(fallback);
}

MenuItem parse_item(pugi::xml_node node)
{
    MenuItem item;
    item.name = attribute_or(node, "name", "No name");
    item.id = attribute_or(node, "id", "No id");
    item.checkable = attribute_or(node, "checkable", "No checkable");
    item.shortcut = attribute_or(node, "shortcut", "No shortcut");
    item.toolbar = attribute_or(node, "toolbar", "No toolbar");
    item.icon = attribute_or(node, "icon", "No icon");
    item.tooltip = attribute_or(node, "tooltip", "No tooltip");
    return item;
}

MenuEntry parse_submenu(pugi::xml_node node)
{
    MenuEntry entry;
    entry.is_submenu = true;
    entry.submenu_name = attribute_or(node, "name", "No name");
    for (pugi::xml_node sub_item : node.children("menuitem"))
    {
        entry.submenu_items.push_back(parse_item(sub_item));
    }
    return entry;
}

} // namespace

Result<MenuDefinition> load_menu_definition(const ConfigPaths& paths, IFileRepository& file_repository)
{
    Result<std::vector<std::uint8_t>> bytes = file_repository.read(paths.menu_file);
    if (!bytes.has_value())
    {
        return std::unexpected(bytes.error());
    }

    pugi::xml_document doc;
    if (pugi::xml_parse_result parsed = doc.load_buffer(bytes->data(), bytes->size()); !parsed)
    {
        return fail(ErrorKind::InvalidConfig, std::format("menu parse error: {}", parsed.description()));
    }

    MenuDefinition definition;
    // An absent <config> or <ecu_menu_definitions> yields empty node sets
    // here, so this loop simply does not run -- matching legacy, which
    // built no menus and reported no error.
    for (pugi::xml_node menu : doc.child("config").child("ecu_menu_definitions").children("menu"))
    {
        Menu parsed_menu;
        parsed_menu.name = attribute_or(menu, "name", "No name");
        // Iterate every child node, not children("menuitem"), so that a
        // submenu and the items around it keep document order. Comments and
        // text nodes report an empty name() and fall through both branches.
        for (pugi::xml_node child : menu.children())
        {
            const std::string_view tag(child.name());
            if (tag == "menu")
            {
                parsed_menu.entries.push_back(parse_submenu(child));
            }
            else if (tag == "menuitem")
            {
                MenuEntry entry;
                entry.item = parse_item(child);
                parsed_menu.entries.push_back(std::move(entry));
            }
        }
        definition.push_back(std::move(parsed_menu));
    }
    return definition;
}

} // namespace fastecu::config
```

- [ ] **Step 5: Add the Bazel targets**

In `src/backend/config/BUILD.bazel`, after the `car_model_catalog_test` target, add:

```python
cc_library(
    name = "menu_definition",
    srcs = ["menu_definition.cpp"],
    hdrs = ["menu_definition.h"],
    deps = [
        ":config_paths",
        "//src/backend/ports",
        "@pugixml",
    ],
)

fastecu_portable_gtest(
    name = "menu_definition_test",
    srcs = ["menu_definition_test.cpp"],
    deps = [
        ":menu_definition",
        "//src/backend/ports/testing:in_memory_file_repository",
    ],
)
```

The package's `default_visibility` already lists `//src/ui:__subpackages__`, so Task 3's UI target can depend on this without a visibility change.

- [ ] **Step 6: Register the target in both portable-closure lists**

In the root `BUILD.bazel`, in the portable `genquery`'s target list, add after `"//src/backend/config:provisioning",`:

```python
        "//src/backend/config:menu_definition",
```

In `scripts/check-portable-closure.py`, in the `ROOT / "src/backend/config"` entry, add to the set:

```python
        "menu_definition",
```

Both are required — the check reads them independently and fails if a portable target is registered in only one.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/config:menu_definition_test //:portable_closure`
Expected: PASS. Six `MenuDefinitionTest` cases pass, and `portable_closure` reports one more portable target with no Qt reachable.

- [ ] **Step 8: Commit**

```bash
prek run --all-files
git add src/backend/config/menu_definition.h src/backend/config/menu_definition.cpp \
        src/backend/config/menu_definition_test.cpp src/backend/config/BUILD.bazel \
        BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat(config): add portable menu.cfg parser

Extracts the parsing half of FileActions::read_menu_file into
load_menu_definition, mirroring load_protocol_catalog. Preserves the
legacy sentinel attribute defaults and the literal-\"true\" predicates,
which the builder branches on."
```

---

### Task 2: Golden test against the shipped `menu.cfg`

**Files:**
- Modify: `src/backend/config/menu_definition_test.cpp`
- Modify: `src/backend/config/BUILD.bazel` (add `data` and `env` to the test target)
- Modify: `resources/shared/BUILD.bazel` (export `config/menu.cfg`)

**Interfaces:**
- Consumes: everything Task 1 produced, plus the `MENU_CFG_PATH` environment variable that Bazel sets from `$(location ...)`.
- Produces: nothing new for later tasks. This task pins the parser against real shipped input so Task 3 and Task 4 can rely on it.

The shipped `resources/shared/config/menu.cfg` has 84 lines, six live top-level menus (`File`, `Edit`, `Tune`, `Ecu`, `View`, `Testing`, `Help`), no submenus at all, one commented-out block at the end of `<menu name="View">`, and a `<popup_menu_definitions>` section that must be ignored.

- [ ] **Step 1: Write the failing tests**

Append to `src/backend/config/menu_definition_test.cpp`. Add `#include <cstdlib>`, `#include <fstream>`, `#include <ios>` and `#include <iterator>` to the include block, then:

```cpp
namespace
{
std::vector<std::uint8_t> read_shipped_menu_cfg()
{
    const char *path = std::getenv("MENU_CFG_PATH");
    EXPECT_NE(path, nullptr) << "MENU_CFG_PATH must be set by the Bazel target's env";
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open()) << "cannot open " << (path ? path : "(null)");
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

MenuDefinition shipped_definition(InMemoryFileRepository& repository)
{
    repository.files["menu.cfg"] = read_shipped_menu_cfg();
    auto definition = load_menu_definition(test_paths(), repository);
    EXPECT_TRUE(definition.has_value());
    return definition.value_or(MenuDefinition{});
}

std::vector<std::string> ids_of(const fastecu::config::Menu& menu)
{
    std::vector<std::string> ids;
    for (const auto& entry : menu.entries)
    {
        ids.push_back(entry.is_submenu ? entry.submenu_name : entry.item.id);
    }
    return ids;
}
} // namespace

TEST(ShippedMenuCfgTest, HasTheSevenTopLevelMenusInOrder)
{
    InMemoryFileRepository repository;
    const MenuDefinition definition = shipped_definition(repository);

    std::vector<std::string> names;
    for (const auto& menu : definition)
    {
        names.push_back(menu.name);
    }
    EXPECT_THAT(names, testing::ElementsAre("File", "Edit", "Tune", "Ecu", "View", "Testing", "Help"));
}

TEST(ShippedMenuCfgTest, FileMenuMatchesTheShippedItemsAndOrder)
{
    InMemoryFileRepository repository;
    const MenuDefinition definition = shipped_definition(repository);
    ASSERT_FALSE(definition.empty());

    EXPECT_THAT(ids_of(definition[0]),
                testing::ElementsAre("open_calibration", "save_calibration", "save_calibration_as", "separator",
                                     "close_calibration", "separator", "quit"));
    EXPECT_TRUE(definition[0].entries[0].item.on_toolbar());
    EXPECT_FALSE(definition[0].entries[2].item.on_toolbar());
    EXPECT_EQ(definition[0].entries[6].item.shortcut, "Ctrl+Q");
}

TEST(ShippedMenuCfgTest, EcuMenuCarriesTheCheckableLoggingToggles)
{
    InMemoryFileRepository repository;
    const MenuDefinition definition = shipped_definition(repository);
    ASSERT_GE(definition.size(), 4U);

    const auto& ecu = definition[3];
    ASSERT_EQ(ecu.name, "Ecu");
    std::vector<std::string> checkable_ids;
    for (const auto& entry : ecu.entries)
    {
        if (!entry.is_submenu && entry.item.is_checkable())
        {
            checkable_ids.push_back(entry.item.id);
        }
    }
    EXPECT_THAT(checkable_ids, testing::ElementsAre("toggle_realtime", "log_to_file"));
}

// The only comment block in the shipped file is the last thing inside
// <menu name="View">, so pugixml (which skips comments and continues) and
// legacy QDom (whose nextSibling().toElement() stopped at one) agree here.
// Pinned so the divergence is a decision on record, not a surprise.
TEST(ShippedMenuCfgTest, ViewMenuHasOnlyTheOneLiveItemBeforeItsCommentBlock)
{
    InMemoryFileRepository repository;
    const MenuDefinition definition = shipped_definition(repository);
    ASSERT_GE(definition.size(), 5U);

    const auto& view = definition[4];
    ASSERT_EQ(view.name, "View");
    EXPECT_THAT(ids_of(view), testing::ElementsAre("setlogviews"));
}

TEST(MenuDefinitionTest, CommentsDoNotTruncateTheRemainingItems)
{
    InMemoryFileRepository repository;
    give(repository, R"(<config><ecu_menu_definitions><menu name="Top">
        <menuitem name="First" id="first" />
        <!-- a comment that legacy QDom iteration stopped at -->
        <menuitem name="Second" id="second" />
    </menu></ecu_menu_definitions></config>)");

    auto definition = load_menu_definition(test_paths(), repository);

    ASSERT_TRUE(definition.has_value());
    ASSERT_EQ((*definition)[0].entries.size(), 2U);
    EXPECT_EQ((*definition)[0].entries[0].item.id, "first");
    EXPECT_EQ((*definition)[0].entries[1].item.id, "second");
}

TEST(ShippedMenuCfgTest, PopupMenuDefinitionsSectionIsIgnored)
{
    InMemoryFileRepository repository;
    const MenuDefinition definition = shipped_definition(repository);

    // The shipped <popup_menu_definitions> holds a second <menu name="Edit">.
    // Exactly one Edit menu must survive, from <ecu_menu_definitions>.
    int edit_menus = 0;
    for (const auto& menu : definition)
    {
        if (menu.name == "Edit")
        {
            ++edit_menus;
        }
    }
    EXPECT_EQ(edit_menus, 1);
}

TEST(MenuDefinitionTest, MissingEcuMenuDefinitionsSectionYieldsEmptyNotError)
{
    InMemoryFileRepository repository;
    give(repository, R"(<config name="FastECU"><popup_menu_definitions><menu name="Edit">
        <menuitem name="Copy" id="copy" />
    </menu></popup_menu_definitions></config>)");

    auto definition = load_menu_definition(test_paths(), repository);

    ASSERT_TRUE(definition.has_value());
    EXPECT_TRUE(definition->empty());
}

TEST(MenuDefinitionTest, UnknownChildTagsAreSkipped)
{
    InMemoryFileRepository repository;
    give(repository, R"(<config><ecu_menu_definitions><menu name="Top">
        <menuitem name="Kept" id="kept" />
        <widget name="Unknown" id="unknown" />
    </menu></ecu_menu_definitions></config>)");

    auto definition = load_menu_definition(test_paths(), repository);

    ASSERT_TRUE(definition.has_value());
    ASSERT_EQ((*definition)[0].entries.size(), 1U);
    EXPECT_EQ((*definition)[0].entries[0].item.id, "kept");
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bazel test --config=release //src/backend/config:menu_definition_test`
Expected: FAIL — the `ShippedMenuCfgTest` cases abort on `MENU_CFG_PATH must be set by the Bazel target's env`, because the target has no `data`/`env` yet. The three non-shipped cases (`CommentsDoNotTruncate...`, `MissingEcuMenuDefinitionsSection...`, `UnknownChildTagsAreSkipped`) should already pass.

- [ ] **Step 3: Export `menu.cfg` from the resources package**

In `resources/shared/BUILD.bazel`, change the existing `exports_files` call to list both configs:

```python
exports_files(
    [
        "config/menu.cfg",
        "config/protocols.cfg",
    ],
    visibility = [
        "//resources/shared:__pkg__",
        "//src/backend/config:__pkg__",
    ],
)
```

Do **not** create a nested `resources/shared/config/BUILD.bazel` — the existing comment above this rule explains why: it would turn `config/` into its own package and silently empty `config_resource`'s `glob(["config/*"])`, which fails the build.

- [ ] **Step 4: Wire the fixture into the test target**

In `src/backend/config/BUILD.bazel`, replace the `menu_definition_test` target from Task 1 with:

```python
fastecu_portable_gtest(
    name = "menu_definition_test",
    srcs = ["menu_definition_test.cpp"],
    data = ["//resources/shared:config/menu.cfg"],
    env = {
        "MENU_CFG_PATH": "$(location //resources/shared:config/menu.cfg)",
    },
    deps = [
        ":menu_definition",
        "//src/backend/ports/testing:in_memory_file_repository",
    ],
)
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/config:menu_definition_test --test_output=errors`
Expected: PASS, 14 cases total.

- [ ] **Step 6: Commit**

```bash
prek run --all-files
git add src/backend/config/menu_definition_test.cpp src/backend/config/BUILD.bazel resources/shared/BUILD.bazel
git commit -m "test(config): pin the menu parser against the shipped menu.cfg

Golden-tests the real 84-line resources/shared/config/menu.cfg: seven
top-level menus in order, the File menu's items and separators, the two
checkable Ecu toggles, and that popup_menu_definitions stays ignored.
Also pins the deliberate comment-handling divergence from legacy QDom
iteration, which the shipped file cannot observe."
```

---

### Task 3: `MenuBuilder` in the desktop UI

**Files:**
- Create: `src/ui/desktop/menu/menu_builder.h`
- Create: `src/ui/desktop/menu/menu_builder.cpp`
- Test: `src/ui/desktop/menu/menu_builder_test.cpp`
- Create: `src/ui/desktop/menu/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::config::MenuDefinition`, `Menu`, `MenuEntry`, `MenuItem` and their predicates from Task 1.
- Produces: `QSignalMapper *build_menus(const fastecu::config::MenuDefinition& definition, QMenuBar *menubar, QToolBar *toolBar, QObject *parent)`. The returned mapper is parented to `parent`, every created `QAction` is parented to `parent`, and each non-separator action is mapped to its `id` string. Task 4 calls this.

- [ ] **Step 1: Write the failing test**

Create `src/ui/desktop/menu/menu_builder_test.cpp`:

```cpp
#include "src/ui/desktop/menu/menu_builder.h"

#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QObject>
#include <QToolBar>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using fastecu::config::Menu;
using fastecu::config::MenuDefinition;
using fastecu::config::MenuEntry;
using fastecu::config::MenuItem;

namespace
{
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
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/ui/desktop/menu:menu_builder_test`
Expected: FAIL — the package does not exist yet.

- [ ] **Step 3: Write the builder header**

Create `src/ui/desktop/menu/menu_builder.h`:

```cpp
#pragma once
#include <QMenuBar>
#include <QObject>
#include <QSignalMapper>
#include <QToolBar>

#include "src/backend/config/menu_definition.h"

// Builds the application menu bar and tool bar from a parsed MenuDefinition.
// This is the widget-construction half of the former
// FileActions::read_menu_file; the parsing half is
// fastecu::config::load_menu_definition.
//
// Every created QAction and the returned QSignalMapper are parented to
// `parent`, which owns them. Each non-separator action is mapped to its
// MenuItem::id, so the caller connects QSignalMapper::mappedString once to
// receive every menu command as a string.
QSignalMapper *build_menus(const fastecu::config::MenuDefinition& definition, QMenuBar *menubar, QToolBar *toolBar,
                           QObject *parent);
```

- [ ] **Step 4: Write the builder**

Create `src/ui/desktop/menu/menu_builder.cpp`:

```cpp
#include "src/ui/desktop/menu/menu_builder.h"

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QString>

#include <string>

namespace
{

QString qs(const std::string& text)
{
    return QString::fromStdString(text);
}

// `tooltip_prefix` carries the legacy asymmetry: a submenu's items are
// prefixed with the SUBMENU's name, a top-level item with its OWN name
// (file_actions.cpp:884 and :935).
QAction *make_action(const fastecu::config::MenuItem& item, const QString& tooltip_prefix, QToolBar *toolBar,
                     QSignalMapper *mapper, bool& toolbar_icon_set, QObject *parent)
{
    QAction *action = new QAction(qs(item.name), parent);
    action->setObjectName(qs(item.id));
    action->setShortcut(qs(item.shortcut));
    // Constructed unconditionally, exactly as legacy did: the "No icon"
    // sentinel yields a null QIcon rather than being skipped.
    action->setIcon(QIcon(qs(item.icon)));
    action->setIconVisibleInMenu(true);
    action->setToolTip(tooltip_prefix + "\n\n" + qs(item.tooltip));
    action->setCheckable(item.is_checkable());
    if (item.on_toolbar())
    {
        toolBar->addAction(action);
        toolbar_icon_set = true;
    }
    mapper->setMapping(action, action->objectName());
    QObject::connect(action, &QAction::triggered, mapper, qOverload<>(&QSignalMapper::map));
    return action;
}

} // namespace

QSignalMapper *build_menus(const fastecu::config::MenuDefinition& definition, QMenuBar *menubar, QToolBar *toolBar,
                           QObject *parent)
{
    QSignalMapper *mapper = new QSignalMapper(parent);

    for (const fastecu::config::Menu& menu : definition)
    {
        QMenu *main_menu = menubar->addMenu(qs(menu.name));
        // Reset per top-level menu, and shared with that menu's submenus:
        // one toolbar separator closes each menu that contributed an icon.
        bool toolbar_icon_set = false;

        for (const fastecu::config::MenuEntry& entry : menu.entries)
        {
            if (entry.is_submenu)
            {
                QMenu *sub_menu = main_menu->addMenu(qs(entry.submenu_name));
                for (const fastecu::config::MenuItem& item : entry.submenu_items)
                {
                    if (item.is_separator())
                    {
                        sub_menu->addSeparator();
                        continue;
                    }
                    sub_menu->addAction(
                        make_action(item, qs(entry.submenu_name), toolBar, mapper, toolbar_icon_set, parent));
                }
                continue;
            }

            if (entry.item.is_separator())
            {
                main_menu->addSeparator();
                continue;
            }
            main_menu->addAction(
                make_action(entry.item, qs(entry.item.name), toolBar, mapper, toolbar_icon_set, parent));
        }

        if (toolbar_icon_set)
        {
            toolBar->addSeparator();
        }
    }

    return mapper;
}
```

- [ ] **Step 5: Add the Bazel package**

Create `src/ui/desktop/menu/BUILD.bazel`:

```python
load("//bazel:gtest_targets.bzl", "fastecu_gtest")
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "qt_cc_library")

package(default_visibility = ["//src/ui:__subpackages__"])

# Qt-linked (QMenuBar/QToolBar/QSignalMapper) but declares no QObject of its
# own, so the header goes through normal_hdrs rather than hdrs -- matching
# //src/ui/desktop/checksum:checksum_correction_command.
qt_cc_library(
    name = "menu_builder",
    srcs = ["menu_builder.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["menu_builder.h"],
    deps = QT_DEPS + ["//src/backend/config:menu_definition"],
)

fastecu_gtest(
    name = "menu_builder_test",
    srcs = ["menu_builder_test.cpp"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    deps = [":menu_builder"],
)
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bazel test --config=release //src/ui/desktop/menu:menu_builder_test --test_output=errors`
Expected: PASS, 6 cases.

- [ ] **Step 7: Commit**

```bash
prek run --all-files
git add src/ui/desktop/menu/
git commit -m "feat(ui): add MenuBuilder for the parsed menu definition

Owns the widget half of the former FileActions::read_menu_file: menu and
action construction, the asymmetric tooltip prefixes, one toolbar
separator per contributing menu, and the id signal mapping."
```

---

### Task 4: Wire `MainWindow` and delete `read_menu_file`

**Files:**
- Modify: `src/ui/desktop/mainwindow.cpp:200-205`
- Modify: `src/ui/desktop/BUILD.bazel`
- Modify: `src/backend/definitions/file_actions.h` (delete the declaration and its comment block)
- Modify: `src/backend/definitions/file_actions.cpp:796-1005` (delete the definition)

**Interfaces:**
- Consumes: `fastecu::config::load_menu_definition` (Task 1) and `build_menus` (Task 3).
- Produces: nothing. This task closes the seam and removes the legacy function.

`MainWindow` already holds `configValues` (a `FileActions::ConfigValuesStructure*`) and a `QtFileRepository m_configFileRepository` value member (`mainwindow.h:189`). It needs a `ConfigPaths` to call the parser: `src/backend/config/legacy_config_paths.h` provides `fastecu::config::paths_from_config_values(const fastecu::definitions::ConfigValuesStructure&)`, already on this target's `deps` (`src/ui/desktop/BUILD.bazel:86`). Use it rather than assembling paths by hand. `QMessageBox` is already used in `mainwindow.cpp` (line 181), so no new include is needed for it.

- [ ] **Step 1: Confirm the current call site**

Run: `sed -n '199,206p' src/ui/desktop/mainwindow.cpp`
Expected output:

```cpp
    setSplashScreenProgress("Setting up menus...", 10);
    QSignalMapper *mapper = fileActions->read_menu_file(ui->menubar, ui->toolBar);
#if QT_VERSION >= 0x060000
    connect(mapper, SIGNAL(mappedString(QString)), this, SLOT(menu_action_triggered(QString)));
#elif QT_VERSION >= 0x050000
    connect(mapper, SIGNAL(mapped(QString)), this, SLOT(menu_action_triggered(QString)));
#endif
```

If the line numbers have shifted (the flash drain edits this file), locate it with
`grep -n read_menu_file src/ui/desktop/mainwindow.cpp` and use that line instead.

- [ ] **Step 2: Replace the call site**

In `src/ui/desktop/mainwindow.cpp`, replace the single `read_menu_file` line with:

```cpp
    QSignalMapper *mapper = nullptr;
    {
        const fastecu::config::ConfigPaths menu_paths = fastecu::config::paths_from_config_values(*configValues);
        fastecu::Result<fastecu::config::MenuDefinition> menu_definition =
            fastecu::config::load_menu_definition(menu_paths, m_configFileRepository);
        if (!menu_definition.has_value())
        {
            // The same modal read_menu_file raised itself (file_actions.cpp:813);
            // 6a-3 routes this through IEventSink instead.
            QMessageBox::warning(this, tr("Ecu menu file"),
                                 tr("Unable to open menu config file '") + configValues->menu_file +
                                     tr("' for reading"));
            menu_definition = fastecu::config::MenuDefinition{};
        }
        mapper = build_menus(*menu_definition, ui->menubar, ui->toolBar, this);
    }
```

Add these includes to `src/ui/desktop/mainwindow.cpp`'s include block:

```cpp
#include "src/backend/config/legacy_config_paths.h"
#include "src/backend/config/menu_definition.h"
#include "src/ui/desktop/menu/menu_builder.h"
```

`paths_from_config_values` takes the struct by const reference and
`m_configFileRepository` is a value member (`mainwindow.h:189`), not a
pointer — hence `*configValues` but a bare `m_configFileRepository`.

- [ ] **Step 3: Add the Bazel dependencies**

In `src/ui/desktop/BUILD.bazel`, add to the `deps` list of the target that compiles `mainwindow.cpp` (the one already listing `"//src/backend/config:legacy_config_paths"`):

```python
        "//src/backend/config:menu_definition",
        "//src/ui/desktop/menu:menu_builder",
```

- [ ] **Step 4: Build to verify the wiring compiles**

Run: `bazel build --config=release //:fastecu`
Expected: SUCCESS. `read_menu_file` still exists at this point but has no callers.

- [ ] **Step 5: Delete the legacy declaration**

In `src/backend/definitions/file_actions.h`, delete this block:

```cpp
    /***************************
     * Read software menu file
     * for menu creation
     **************************/
    QSignalMapper *read_menu_file(QMenuBar *menubar, QToolBar *toolBar);
```

- [ ] **Step 6: Delete the legacy definition**

In `src/backend/definitions/file_actions.cpp`, delete the whole
`QSignalMapper *FileActions::read_menu_file(QMenuBar *menubar, QToolBar *toolBar) { ... }`
function — from its opening line through its closing brace, including the
commented-out `popup_menu_definitions` block inside it. It currently spans
lines 796-1005; locate it with `grep -n "FileActions::read_menu_file" src/backend/definitions/file_actions.cpp`.

Then remove the includes in `file_actions.h` that no longer have a user.
Check each one before deleting it:

```bash
grep -n "QSignalMapper\|QMenuBar\|QToolBar\|QMenu\b" src/backend/definitions/*.cpp src/backend/definitions/*.h
```

Delete `#include <QSignalMapper>`, `#include <QMenu>`, `#include <QMenuBar>`,
and `#include <QToolBar>` from `file_actions.h` **only** if that grep shows no
remaining use. Leave every other include alone.

- [ ] **Step 7: Run the full suite**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //...`
Expected: PASS across all targets, including `//:portable_closure`, `//:serial_compat_allowlist`, and `//:openpty_includes`.

- [ ] **Step 8: Verify the parallel-branch constraint holds**

Run:

```bash
git diff --name-only origin/master | grep -E 'src/platform/desktop/common/flash/legacy/|src/ui/desktop/flash/' || echo "OK: no drain-owned files touched"
```

Expected: `OK: no drain-owned files touched`.

- [ ] **Step 9: Commit**

```bash
prek run --all-files
git add src/ui/desktop/mainwindow.cpp src/ui/desktop/BUILD.bazel \
        src/backend/definitions/file_actions.h src/backend/definitions/file_actions.cpp
git commit -m "refactor: build menus from the portable definition, drop read_menu_file

MainWindow now parses menu.cfg through load_menu_definition and builds
widgets through build_menus, removing 210 lines of QMenuBar/QToolBar
construction from src/backend/definitions. The file-open failure keeps
its modal warning for now; 6a-3 routes it through IEventSink."
```

---

## Verification

After Task 4, these all hold:

- `grep -rn "read_menu_file" src/` returns nothing.
- `grep -rn "QDomDocument" src/backend/definitions/file_actions.cpp` still returns hits — other functions in this file legitimately use it, and they are out of scope for 6a-1.
- `bazel test --config=release //...` is green.
- `git diff --name-only origin/master` lists no path under `src/platform/desktop/common/flash/legacy/` or `src/ui/desktop/flash/`.
- `src/backend/definitions/file_actions.cpp` is about 210 lines shorter, down from 1562 to roughly 1350.
