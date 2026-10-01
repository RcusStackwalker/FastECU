# Static Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the runtime-built, `menu.cfg`-driven menu with a compiled-in menu declared in `mainwindow.ui`, with direct typed `QAction` access and no text-keyed lookups.

**Architecture:** Pin the current menu and behavior with tests first. Then declare all 31 actions, 7 menus and the toolbar in `mainwindow.ui` (generated once from `menu.cfg` by a throwaway script), connect each action directly to its handler, replace the six text-scan sites with `ui->actionX`, and finally delete the runtime machinery (`build_menus`, `menu_definition`, `MenuCommand`, `menu.cfg`).

**Tech Stack:** C++23, Qt 6 (`uic`, `QAction`, `QKeySequence`), Bazel, GoogleTest.

**Spec:** [2026-10-01-static-menu-design.md](../specs/2026-10-01-static-menu-design.md)

## Global Constraints

- Bazel is the only build: `bazel test --config=release <target>`. Run `python3 scripts/gazelle_check.py --fix` after BUILD-affecting changes and review its output; no new whole-rule `# keep` on a C++ library or binary.
- Work on a feature branch, never `master` (`prek` refuses commits on `master`). The spec branch `docs/typed-menu-actions-spec` already holds the spec and this plan; implement on a new branch cut from it, `refactor/static-menu`.
- The `qt_layer` package group in `bazel/qt/BUILD.bazel` gets no new entry.
- Tests are GoogleTest, package-owned and co-located. Mocks and fakes are package-owned (`.../testing/` subpackage pattern, see `src/backend/ports/testing/BUILD.bazel`).
- No `#ifdef` branches in shared sources for platform differences; shortcut fallbacks are decided at runtime from `QKeySequence`, not by preprocessor.
- Cross-document references in Markdown are links with human-readable text, not backticked paths.
- Default shortcuts that stay literal: `F3`, `F4`, `Space`, `+`, `-`, `Ctrl++`, `Ctrl+-`, `S`, `Ctrl+H`, `Ctrl+J`, `Ctrl+I`, `Ctrl+L`. Standard-key-with-fallback: Open `Ctrl+O`, Save `Ctrl+S`, Save As `Ctrl+Shift+S`, Copy `Ctrl+C`, Paste `Ctrl+V`, Quit `Ctrl+Q`.
- No ECU I/O, protocol or address-guard code is touched.
- Commit messages end with:
  `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01FgiHcLMaxFbqt4VFC1xFeN`

## Review Focus

1. **Stale or malformed `menu.cfg` in an existing user's config directory.** The window must start normally and ignore it. (Task 3 test.)
2. **Actions disabled while identification runs.** Logging and Connect are disabled, Disconnect stays enabled, all restored afterwards. (Task 2 test, kept in Task 3.)
3. **Toolbar order.** Menu actions (with their two separators) come before the transport and port widgets that `MainWindow` appends. (Task 3 test.)
4. **Shortcut collisions and platform gaps.** No two actions share a key sequence, and every action that had a shortcut still has a non-empty one on the host platform, even where Qt has no standard key (`SaveAs`, `Quit` on Windows). (Task 3 test.)
5. **Checkable state through the real action.** Triggering Logging reaches `toggle_realtime` with `logging_state` following the action's checked state; `restoreLoggingUiState` un-checks it. (Task 2 test, kept in Task 3.)

---

## File Structure

| File | Responsibility |
|---|---|
| `src/ui/desktop/menu/testing/menu_snapshot.{h,cpp}` (new) | Test-only: render a `QMenuBar` + `QToolBar` as stable text |
| `src/ui/desktop/menu/legacy_menu_golden_test.cpp` (new, deleted in Task 4) | Pin the runtime-built menu |
| `src/ui/desktop/menu/testdata/legacy_menu.golden` (new, deleted in Task 4) | Its expected output |
| `src/ui/desktop/mainwindow.ui` | All menus, actions, toolbar membership |
| `src/ui/desktop/widgets/menu_actions.cpp` | `connect_menu_actions`, `apply_standard_shortcuts`, `show_about_dialog`, the rewritten toggle/identification handlers |
| `src/ui/desktop/widgets/mainwindow.{h,cpp}` | Drop mapper/builder; rewrite `set_flash_arrow_state`, `restoreLoggingUiState` |
| `src/ui/desktop/widgets/mainwindow_test.cpp` | Behavior pins, static-menu golden test, migrated callers |
| `src/ui/desktop/widgets/testdata/main_menu.golden` (new) | Expected output for the static menu |

Deleted in Task 4: `src/ui/desktop/menu/menu_builder*`, `src/backend/config/menu_definition*`, `ConfigPaths::menu_file`, `src/algorithms/menu/`, `resources/shared/config/menu.cfg`.

---

### Task 1: Pin the runtime-built menu with a golden snapshot

**Files:**
- Create: `src/ui/desktop/menu/testing/menu_snapshot.h`
- Create: `src/ui/desktop/menu/testing/menu_snapshot.cpp`
- Create: `src/ui/desktop/menu/testing/BUILD.bazel`
- Create: `src/ui/desktop/menu/legacy_menu_golden_test.cpp`
- Create: `src/ui/desktop/menu/testdata/legacy_menu.golden`
- Modify: `src/ui/desktop/menu/BUILD.bazel`

**Interfaces:**
- Produces: `std::string fastecu::ui::testing::menu_snapshot(const QMenuBar&, const QToolBar&)` in `//src/ui/desktop/menu/testing:menu_snapshot`. Task 3 reuses it.

- [ ] **Step 1: Cut the implementation branch**

```bash
git checkout docs/typed-menu-actions-spec
git checkout -b refactor/static-menu
```

- [ ] **Step 2: Write the snapshot helper**

`src/ui/desktop/menu/testing/menu_snapshot.h`:

```cpp
#pragma once

#include <string>

class QMenuBar;
class QToolBar;

namespace fastecu::ui::testing
{

// A stable, line-per-action text rendering of a menu bar and the actions of a
// toolbar, for golden comparison. Per action: text, portable shortcut text,
// checkable flag, whether the icon is non-null, and the tooltip. Submenus nest
// with two spaces per level; separators render as "---". Toolbar entries that
// host a widget (a QWidgetAction: comboboxes, buttons, spacers) are skipped:
// they belong to MainWindow, not to the menu.
std::string menu_snapshot(const QMenuBar& menubar, const QToolBar& toolbar);

} // namespace fastecu::ui::testing
```

`src/ui/desktop/menu/testing/menu_snapshot.cpp`:

```cpp
#include "src/ui/desktop/menu/testing/menu_snapshot.h"

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QString>
#include <QStringList>
#include <QToolBar>
#include <QWidgetAction>

#include <string>

namespace
{

QString shortcut_text(const QAction& action)
{
    QStringList parts;
    for (const QKeySequence& sequence : action.shortcuts())
    {
        parts << sequence.toString(QKeySequence::PortableText);
    }
    return parts.join(QLatin1Char(';'));
}

QString tooltip_text(QString tooltip)
{
    tooltip.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return tooltip;
}

void append_line(std::string& out, int depth, const QString& text)
{
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    out += text.toStdString();
    out += '\n';
}

void append_action(std::string& out, int depth, const QAction& action)
{
    if (action.isSeparator())
    {
        append_line(out, depth, QStringLiteral("---"));
        return;
    }
    append_line(out, depth,
                QStringLiteral("%1 | keys=%2 | checkable=%3 | icon=%4 | tip=%5")
                    .arg(action.text(), shortcut_text(action), action.isCheckable() ? QStringLiteral("1") : QStringLiteral("0"),
                         action.icon().isNull() ? QStringLiteral("0") : QStringLiteral("1"), tooltip_text(action.toolTip())));
}

void append_menu(std::string& out, int depth, const QMenu& menu)
{
    append_line(out, depth, QStringLiteral("[%1]").arg(menu.title()));
    for (const QAction *action : menu.actions())
    {
        if (action->menu() != nullptr)
        {
            append_menu(out, depth + 1, *action->menu());
            continue;
        }
        append_action(out, depth + 1, *action);
    }
}

} // namespace

namespace fastecu::ui::testing
{

std::string menu_snapshot(const QMenuBar& menubar, const QToolBar& toolbar)
{
    std::string out;
    for (const QAction *top : menubar.actions())
    {
        if (top->menu() != nullptr)
        {
            append_menu(out, 0, *top->menu());
        }
    }
    append_line(out, 0, QStringLiteral("[toolbar]"));
    for (const QAction *action : toolbar.actions())
    {
        if (qobject_cast<const QWidgetAction *>(action) != nullptr)
        {
            continue;
        }
        append_action(out, 1, *action);
    }
    return out;
}

} // namespace fastecu::ui::testing
```

`src/ui/desktop/menu/testing/BUILD.bazel` (model the testonly/visibility lines on `src/backend/ports/testing/BUILD.bazel`; read that file first and copy its `package(...)`/`testonly` pattern):

```starlark
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//bazel:qt_common.bzl", "COMMON_COPTS")

package(default_visibility = ["//bazel/layers:ui"])

cc_library(
    name = "menu_snapshot",
    testonly = True,
    srcs = ["menu_snapshot.cpp"],
    hdrs = ["menu_snapshot.h"],
    copts = COMMON_COPTS,
    implementation_deps = ["//bazel/qt:gui"],
    deps = [
        "//bazel/qt:core",
        "//bazel/qt:widgets",
    ],
)
```

- [ ] **Step 3: Write the golden test**

`src/ui/desktop/menu/legacy_menu_golden_test.cpp`:

```cpp
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
```

- [ ] **Step 4: Add the Bazel target**

Append to `src/ui/desktop/menu/BUILD.bazel`:

```starlark
fastecu_gtest(
    name = "legacy_menu_golden_test",
    srcs = ["legacy_menu_golden_test.cpp"],
    data = [
        "testdata/legacy_menu.golden",
        "//resources/shared:config/menu.cfg",
    ],
    env = {
        "MENU_CFG_PATH": "$(location //resources/shared:config/menu.cfg)",
        "MENU_GOLDEN_PATH": "$(location testdata/legacy_menu.golden)",
        "QT_QPA_PLATFORM": "offscreen",
    },
    deps = [
        # Icon resources register at link time; the test checks icons resolve.
        "//resources/desktop:icons_resource",  # keep
        ":menu_builder",
        "//bazel/qt:core",
        "//bazel/qt:widgets",
        "//src/backend/config:menu_definition",
        "//src/backend/ports/testing:in_memory_file_repository",
        "//src/backend/ports/testing:result_matchers",
        "//src/ui/desktop/menu/testing:menu_snapshot",
        "@googletest//:gtest",
    ],
)
```

Create an empty golden: `: > src/ui/desktop/menu/testdata/legacy_menu.golden`

- [ ] **Step 5: Run it; it must fail and print the actual snapshot**

Run: `bazel test --config=release //src/ui/desktop/menu:legacy_menu_golden_test --test_output=all`
Expected: FAIL, with `actual snapshot:` followed by the rendering.

- [ ] **Step 6: Capture and check the golden**

Copy the text after `actual snapshot:` into `src/ui/desktop/menu/testdata/legacy_menu.golden` verbatim (trailing newline included). Then check it against the known shape before trusting it:

- 7 top-level `[...]` menus in order: File, Edit, Tune, Ecu, View, Testing, Help.
- Exactly 31 non-separator action lines in the menus.
- Exactly 9 lines contain `icon=0`: Set value, Interpolate bidirectional, Log views, and the six Testing-menu items (Diagnostic Trouble Codes, Hex Editor, Terminal, BIU communication, Get Encryption Key, WinOLS CSV to RomRaider XML). If a different set is `icon=0`, stop and investigate: an icon path resolves differently than the spec assumes.
- `[toolbar]` lists exactly: Open calibration, Save calibration, `---`, Logging, Log to file, Read from ecu, Test write to ecu, Write to ecu, `---`.
- Tooltips carry the legacy prefix, e.g. `tip=Open calibration\n\nOpen calibration file.`

- [ ] **Step 7: Run again to verify it passes**

Run: `bazel test --config=release //src/ui/desktop/menu:legacy_menu_golden_test`
Expected: PASS

- [ ] **Step 8: Gazelle and commit**

```bash
python3 scripts/gazelle_check.py --fix
git add src/ui/desktop/menu
git commit -m "test: pin the runtime-built menu with a golden snapshot

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FgiHcLMaxFbqt4VFC1xFeN"
```

---

### Task 2: Pin menu-driven behavior in `MainWindow` on the current build

**Files:**
- Modify: `src/ui/desktop/widgets/mainwindow_test.cpp` (fixture menu file at ~line 573; `triggerMenu` at ~line 285; invoke sites ~2108, ~2652, ~2757; new tests)

**Interfaces:**
- Produces, in `mainwindow_test.cpp`: `struct ActionName { const char *legacy_id; const char *member; }`; constants `kToggleRealtime`, `kLogToFile`, `kConnectToEcu`, `kDisconnectFromEcu`, `kReadRomFromEcu`, `kTestWriteRomToEcu`, `kWriteRomToEcu`, `kDtcWindow`, `kBiuCommunication`, `kTerminal`; `QAction *menuAction(MainWindow&, const ActionName&)`; `bool triggerMenu(MainWindow&, const ActionName&)`. Task 3 changes only the bodies of the last two.

- [ ] **Step 1: Let the fixture provision the real menu**

In `mainwindow_test.cpp`, delete the block that writes the empty `menu.cfg` (the `ASSERT_TRUE(writeTextFile(config_dir + "menu.cfg", R"(...)"));` statement, ~lines 573-579). `TestServices` provisions the shipped bundle for any file the fixture did not write, so the window now builds the real menu.

- [ ] **Step 2: Add the action names and lookup helpers**

Replace the existing `triggerMenu` (~line 285) with:

```cpp
// An action as the tests name it: `legacy_id` is the runtime-built menu's
// objectName (menu.cfg id), `member` the name it has once the menu is static.
struct ActionName
{
    const char *legacy_id;
    const char *member;
};

constexpr ActionName kToggleRealtime{"toggle_realtime", "actionToggleRealtime"};
constexpr ActionName kLogToFile{"log_to_file", "actionLogToFile"};
constexpr ActionName kConnectToEcu{"connect_to_ecu", "actionConnectToEcu"};
constexpr ActionName kDisconnectFromEcu{"disconnect_from_ecu", "actionDisconnectFromEcu"};
constexpr ActionName kReadRomFromEcu{"read_rom_from_ecu", "actionReadRomFromEcu"};
constexpr ActionName kTestWriteRomToEcu{"test_write_rom_to_ecu", "actionTestWriteRomToEcu"};
constexpr ActionName kWriteRomToEcu{"write_rom_to_ecu", "actionWriteRomToEcu"};
constexpr ActionName kDtcWindow{"dtc_window", "actionDtcWindow"};
constexpr ActionName kBiuCommunication{"biu_communication", "actionBiuCommunication"};
constexpr ActionName kTerminal{"terminal", "actionTerminal"};

QAction *menuAction(MainWindow& window, const ActionName& name)
{
    return window.findChild<QAction *>(QString::fromLatin1(name.legacy_id));
}

bool triggerMenu(MainWindow& window, const ActionName& name)
{
    return QMetaObject::invokeMethod(&window, "menu_action_triggered", Qt::DirectConnection,
                                     Q_ARG(QString, QString::fromLatin1(name.legacy_id)));
}
```

- [ ] **Step 3: Migrate the callers to `ActionName`**

Replace every call; there are exactly these:

- `triggerMenu(window, "connect_to_ecu")` (6 sites) → `triggerMenu(window, kConnectToEcu)`
- `triggerMenu(window, "disconnect_from_ecu")` → `triggerMenu(window, kDisconnectFromEcu)`
- `triggerMenu(window, "toggle_realtime")` → `triggerMenu(window, kToggleRealtime)`
- `triggerMenu(window, "biu_communication")` → `triggerMenu(window, kBiuCommunication)`
- the three raw `QMetaObject::invokeMethod(&window, "menu_action_triggered", ... Q_ARG(QString, QStringLiteral("toggle_realtime")))` sites (~2108, ~2757, and the loop at ~2650 over `{"dtc_window", "biu_communication", "terminal"}`) → `triggerMenu(window, kToggleRealtime)`, `triggerMenu(window, kDisconnectFromEcu)`, and a loop over `{kDtcWindow, kBiuCommunication, kTerminal}` respectively, keeping each surrounding `ASSERT_TRUE(...)`.

Run: `grep -n 'menu_action_triggered\|triggerMenu(' src/ui/desktop/widgets/mainwindow_test.cpp`
Expected: only the helper definition invokes `menu_action_triggered`.

- [ ] **Step 4: Run the whole suite as a baseline**

Run: `bazel test --config=release //src/ui/desktop/widgets:test_mainwindow`
Expected: PASS. If a test fails only because the real menu now exists (for example it counted toolbar actions), fix that test's expectation and say why in the commit message; it is a fixture change, not a behavior change.

- [ ] **Step 5: Write the behavior pins**

Add as `check_` members, following the existing pattern (declare in the `protected:` list of `MainWindowTest`, define after it, call from a `TEST_F` that wraps `ASSERT_NO_FATAL_FAILURE`). Use these bodies.

```cpp
void MainWindowTest::check_restoreLoggingUiStateUnchecksLogging()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QAction *logging = menuAction(window, kToggleRealtime);
    ASSERT_NE(logging, nullptr);
    ASSERT_TRUE(logging->isCheckable());
    logging->setChecked(true);
    window.logging_state = true;

    window.restoreLoggingUiState();

    EXPECT_FALSE(logging->isChecked());
    EXPECT_FALSE(window.logging_state);
}

void MainWindowTest::check_setRealtimeStateChecksAndUnchecksLogging()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QAction *logging = menuAction(window, kToggleRealtime);
    ASSERT_NE(logging, nullptr);

    window.set_realtime_state(true);
    EXPECT_TRUE(logging->isChecked());
    window.set_realtime_state(false);
    EXPECT_FALSE(logging->isChecked());
}

void MainWindowTest::check_identificationDisablesLoggingAndConnectButNotDisconnect()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QAction *logging = menuAction(window, kToggleRealtime);
    QAction *connect_action = menuAction(window, kConnectToEcu);
    QAction *disconnect_action = menuAction(window, kDisconnectFromEcu);
    ASSERT_NE(logging, nullptr);
    ASSERT_NE(connect_action, nullptr);
    ASSERT_NE(disconnect_action, nullptr);

    window.set_identification_in_progress(true);
    EXPECT_FALSE(logging->isEnabled());
    EXPECT_FALSE(connect_action->isEnabled());
    EXPECT_TRUE(disconnect_action->isEnabled());

    window.set_identification_in_progress(false);
    EXPECT_TRUE(logging->isEnabled());
    EXPECT_TRUE(connect_action->isEnabled());
    EXPECT_TRUE(disconnect_action->isEnabled());
}

void MainWindowTest::check_logToFileActionDrivesWriteDatalogToFile()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QAction *log_to_file = menuAction(window, kLogToFile);
    ASSERT_NE(log_to_file, nullptr);
    ASSERT_TRUE(log_to_file->isCheckable());

    log_to_file->setChecked(true);
    window.toggle_log_to_file();
    EXPECT_TRUE(window.write_datalog_to_file);

    log_to_file->setChecked(false);
    window.toggle_log_to_file();
    EXPECT_FALSE(window.write_datalog_to_file);
}
```

Extend the existing flash-arrow test (`check_unmatchedRomFlashMethodChangesNothing` is not it; the test is the one at ~line 2538 containing `"Test flash"` and `set_flash_arrow_state`). Replace its stub setup:

```cpp
    auto *menu = window.ui->menubar->addMenu("Test flash");
    const QList<QAction *> actions{menu->addAction("Read from ecu"), menu->addAction("Test write to ecu"),
                                   menu->addAction("Write to ecu")};
```
with
```cpp
    const QList<QAction *> actions{menuAction(window, kReadRomFromEcu), menuAction(window, kTestWriteRomToEcu),
                                   menuAction(window, kWriteRomToEcu)};
    for (QAction *action : actions)
    {
        ASSERT_NE(action, nullptr);
    }
```
and delete the stale comment above it (`The fixture's menu.cfg is empty; ...`). Do the same for the two `"Test logging"`/`"Logging"` stubs (`prepareLogging` returns the real action; the second at ~line 3203 uses `menuAction(window, kToggleRealtime)`), keeping `action->setCheckable(true)` out since the real action is already checkable.

Register the four new checks:

```cpp
TEST_F(MainWindowTest, restoreLoggingUiStateUnchecksLogging)
{
    ASSERT_NO_FATAL_FAILURE(check_restoreLoggingUiStateUnchecksLogging());
}
// ...same wrapper for the other three...
```

- [ ] **Step 6: Run to verify they pass on the current build**

Run: `bazel test --config=release //src/ui/desktop/widgets:test_mainwindow`
Expected: PASS. These pin existing behavior, so they must pass before any production change. A failure means the test is wrong, not the code.

- [ ] **Step 7: Commit**

```bash
git add src/ui/desktop/widgets/mainwindow_test.cpp
git commit -m "test: pin menu-driven MainWindow behavior against the shipped menu

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FgiHcLMaxFbqt4VFC1xFeN"
```

---

### Task 3: Declare the menu in `mainwindow.ui` and connect it directly

**Files:**
- Modify: `src/ui/desktop/mainwindow.ui`
- Modify: `src/ui/desktop/widgets/mainwindow.h`, `mainwindow.cpp`, `menu_actions.cpp`
- Modify: `src/ui/desktop/widgets/mainwindow_test.cpp`
- Modify: `src/ui/desktop/widgets/BUILD.bazel`
- Create: `src/ui/desktop/widgets/testdata/main_menu.golden`
- Scratch (not committed): `<scratchpad>/generate_menu_ui.py`

**Interfaces:**
- Consumes: Task 1's `menu_snapshot`; Task 2's `ActionName`, `menuAction`, `triggerMenu`.
- Produces: `ui->actionOpenCalibration`, `actionSaveCalibration`, `actionSaveCalibrationAs`, `actionCloseCalibration`, `actionQuit`, `actionCopy`, `actionPaste`, `actionSettings`, `actionCoarseIncrement`, `actionCoarseDecrement`, `actionFineIncrement`, `actionFineDecrement`, `actionSetValue`, `actionInterpolateHorizontal`, `actionInterpolateVertical`, `actionInterpolateBidirectional`, `actionConnectToEcu`, `actionDisconnectFromEcu`, `actionToggleRealtime`, `actionLogToFile`, `actionReadRomFromEcu`, `actionTestWriteRomToEcu`, `actionWriteRomToEcu`, `actionSetLogViews`, `actionDtcWindow`, `actionHexEditor`, `actionTerminal`, `actionBiuCommunication`, `actionGetKey`, `actionWinolsCsvToRomRaiderXml`, `actionAbout`; menus `menuFile`, `menuEdit`, `menuTune`, `menuEcu`, `menuView`, `menuTesting`, `menuHelp`.

- [ ] **Step 1: Write the one-off generator**

Save as `generate_menu_ui.py` in the scratchpad directory. It reads `menu.cfg` (so nothing is hand-typed) and rewrites the menu bar, toolbar and action list of `mainwindow.ui`:

```python
import re
import sys
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

CFG = "resources/shared/config/menu.cfg"
UI = "src/ui/desktop/mainwindow.ui"

NAMES = {
    "open_calibration": "OpenCalibration", "save_calibration": "SaveCalibration",
    "save_calibration_as": "SaveCalibrationAs", "close_calibration": "CloseCalibration",
    "quit": "Quit", "copy": "Copy", "paste": "Paste", "settings": "Settings",
    "coarse_inc": "CoarseIncrement", "coarse_dec": "CoarseDecrement",
    "fine_inc": "FineIncrement", "fine_dec": "FineDecrement", "set_value": "SetValue",
    "interpolate_horizontal": "InterpolateHorizontal",
    "interpolate_vertical": "InterpolateVertical",
    "interpolate_bidirectional": "InterpolateBidirectional",
    "connect_to_ecu": "ConnectToEcu", "disconnect_from_ecu": "DisconnectFromEcu",
    "toggle_realtime": "ToggleRealtime", "log_to_file": "LogToFile",
    "read_rom_from_ecu": "ReadRomFromEcu", "test_write_rom_to_ecu": "TestWriteRomToEcu",
    "write_rom_to_ecu": "WriteRomToEcu", "setlogviews": "SetLogViews",
    "dtc_window": "DtcWindow", "hex_editor": "HexEditor", "terminal": "Terminal",
    "biu_communication": "BiuCommunication", "get_key": "GetKey",
    "winols_csv_to_romraider_xml": "WinolsCsvToRomRaiderXml", "about": "About",
}
# Bound in code (apply_standard_shortcuts), so no literal shortcut in the .ui.
STANDARD_KEY = {"open_calibration", "save_calibration", "save_calibration_as", "copy", "paste", "quit"}
ROLES = {"quit": "QuitRole", "about": "AboutRole", "settings": "PreferencesRole"}
ICON_FIX = {  # the doubled-path icon never resolved
    "dtc_window": ":/icons/utilities-system-monitor.png",
    "hex_editor": "", "terminal": "", "biu_communication": "", "get_key": "",
    "winols_csv_to_romraider_xml": "",
}

root = ET.parse(CFG).getroot().find("ecu_menu_definitions")
menus, actions, toolbar, toolbar_tail = [], [], [], []
for menu in root.findall("menu"):
    entries, had_toolbar_icon = [], False
    for item in menu.findall("menuitem"):
        a = item.attrib
        if a["id"] == "separator":
            entries.append(None)
            continue
        entries.append(a["id"])
        actions.append(a)
        if a["toolbar"] == "true":
            toolbar.append(a["id"])
            had_toolbar_icon = True
    if had_toolbar_icon:
        toolbar.append(None)  # one separator closes each menu that contributed
    menus.append((menu.get("name"), entries))

assert len(actions) == 31, len(actions)
assert all(a["id"] in NAMES for a in actions)

def member(action_id):
    return "action" + NAMES[action_id]

def add(lines, indent, ident):
    pad = " " * indent
    lines.append(f'{pad}<addaction name="{"separator" if ident is None else member(ident)}"/>')

out = []
out.append('  <widget class="QMenuBar" name="menubar">')
out.append('   <property name="geometry">')
out.append('    <rect>\n     <x>0</x>\n     <y>0</y>\n     <width>1024</width>\n     <height>20</height>\n    </rect>')
out.append('   </property>')
for title, entries in menus:
    out.append(f'   <widget class="QMenu" name="menu{title}">')
    out.append('    <property name="title">')
    out.append(f'     <string>{escape(title)}</string>')
    out.append('    </property>')
    for ident in entries:
        add(out, 4, ident)
    out.append('   </widget>')
for title, _ in menus:
    out.append(f'   <addaction name="menu{title}"/>')
out.append('  </widget>')
menubar_xml = "\n".join(out)

tb = []
for ident in toolbar:
    add(tb, 3, ident)
toolbar_xml = "\n".join(tb)

acts = []
for a in actions:
    ident = a["id"]
    acts.append(f' <action name="{member(ident)}">')
    icon = ICON_FIX.get(ident, a["icon"])
    if icon:
        acts.append('  <property name="icon">')
        acts.append(f'   <iconset><normaloff>{icon}</normaloff>{icon}</iconset>')
        acts.append('  </property>')
    acts.append('  <property name="text">')
    acts.append(f'   <string>{escape(a["name"])}</string>')
    acts.append('  </property>')
    if a["tooltip"]:
        acts.append('  <property name="toolTip">')
        acts.append(f'   <string>{escape(a["tooltip"])}</string>')
        acts.append('  </property>')
    if a["checkable"] == "true":
        acts.append('  <property name="checkable">')
        acts.append('   <bool>true</bool>')
        acts.append('  </property>')
    shortcut = a["shortcut"]
    if shortcut and shortcut != "false" and ident not in STANDARD_KEY:
        acts.append('  <property name="shortcut">')
        acts.append(f'   <string>{escape(shortcut)}</string>')
        acts.append('  </property>')
    if ident in ROLES:
        acts.append('  <property name="menuRole">')
        acts.append(f'   <enum>QAction::{ROLES[ident]}</enum>')
        acts.append('  </property>')
    acts.append(' </action>')
actions_xml = "\n".join(acts)

text = open(UI, encoding="utf-8").read()
text, n = re.subn(r'  <widget class="QMenuBar" name="menubar">.*?\n  </widget>\n', menubar_xml + "\n", text, count=1, flags=re.S)
assert n == 1
marker = '   <attribute name="toolBarBreak">\n    <bool>false</bool>\n   </attribute>\n'
assert text.count(marker) == 1
text = text.replace(marker, marker + toolbar_xml + "\n")
assert text.count(" <resources/>") == 1
text = text.replace(" <resources/>", actions_xml + "\n <resources/>")
open(UI, "w", encoding="utf-8").write(text)
print("ok:", len(actions), "actions,", len(menus), "menus,", len(toolbar), "toolbar entries")
```

- [ ] **Step 2: Run it and inspect the result**

Run: `python3 <scratchpad>/generate_menu_ui.py`
Expected: `ok: 31 actions, 7 menus, 9 toolbar entries`

Then: `git diff --stat src/ui/desktop/mainwindow.ui` (one file, insertions only plus the replaced menubar block) and `bazel build --config=release //src/ui/desktop:ui_mainwindow`. Expected: builds, and the generated header declares the 31 `action*` members.

- [ ] **Step 3: Write the failing static-menu tests**

In `mainwindow_test.cpp` add (as `check_` members with `TEST_F` wrappers, same pattern):

```cpp
void MainWindowTest::check_menuMatchesTheGolden()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    const std::string actual = fastecu::ui::testing::menu_snapshot(*window.ui->menubar, *window.ui->toolBar);
    std::ifstream file(std::getenv("MAIN_MENU_GOLDEN_PATH"), std::ios::binary);
    ASSERT_TRUE(file.is_open());
    const std::string expected((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(actual, expected) << "actual snapshot:\n" << actual;
}

void MainWindowTest::check_everyIconNamedByTheMenuResolves()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    int without_icon = 0;
    for (const QAction *action : window.findChildren<QAction *>())
    {
        if (!action->objectName().startsWith(QStringLiteral("action")))
        {
            continue;
        }
        without_icon += action->icon().isNull() ? 1 : 0;
    }
    // Set value, Interpolate bidirectional, Log views, Hex Editor, Terminal,
    // BIU communication, Get Encryption Key and WinOLS CSV have no icon by
    // design; any other null icon is a mistyped path.
    EXPECT_EQ(without_icon, 8);
}

void MainWindowTest::check_noTwoActionsShareAShortcutAndNoneLostItsBinding()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    QSet<QString> seen;
    for (const QAction *action : window.findChildren<QAction *>())
    {
        for (const QKeySequence& sequence : action->shortcuts())
        {
            const QString text = sequence.toString(QKeySequence::PortableText);
            EXPECT_FALSE(seen.contains(text)) << qPrintable(text) << " bound twice";
            seen.insert(text);
        }
    }
    for (const ActionName *name : {&kLogToFile, &kConnectToEcu, &kDisconnectFromEcu, &kToggleRealtime})
    {
        EXPECT_FALSE(menuAction(window, *name)->shortcuts().isEmpty()) << name->member;
    }
    // Open, Save, Save As, Copy, Paste and Quit had shortcuts in menu.cfg. Qt
    // has no standard key for Save As or Quit on Windows, so each must still
    // be bound there through its fallback.
    for (const char *member : {"actionOpenCalibration", "actionSaveCalibration", "actionSaveCalibrationAs",
                               "actionCopy", "actionPaste", "actionQuit"})
    {
        const auto *action = window.findChild<QAction *>(QString::fromLatin1(member));
        ASSERT_NE(action, nullptr) << member;
        EXPECT_FALSE(action->shortcut().isEmpty()) << member;
    }
}

void MainWindowTest::check_toolbarKeepsMenuActionsBeforeTheTransportWidgets()
{
    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    const QList<QAction *> actions = window.ui->toolBar->actions();
    // Open, Save, |, Logging, Log to file, Read, Test write, Write, |, then widgets.
    ASSERT_GE(actions.size(), 10);
    EXPECT_TRUE(actions[2]->isSeparator());
    EXPECT_TRUE(actions[8]->isSeparator());
    // widgetForAction cannot tell these apart (every action has a tool button,
    // separators have a separator widget); only a QWidgetAction hosts a widget
    // that MainWindow added in code.
    for (int i = 0; i < 9; ++i)
    {
        EXPECT_EQ(qobject_cast<QWidgetAction *>(actions[i]), nullptr) << i;
    }
    EXPECT_NE(qobject_cast<QWidgetAction *>(actions[9]), nullptr);
}

void MainWindowTest::check_aStaleOrMalformedMenuCfgIsIgnored()
{
    const QString menu_cfg = config_root_->path() + "/" + QString::fromStdString(kTestApplication.version) + "/config/menu.cfg";
    ASSERT_TRUE(writeTextFile(menu_cfg, "<<< not xml >>>"));

    ModalDriver constructor_driver{QString()};
    constructor_driver.start();
    TestServices services{config_root_->path()};
    ASSERT_TRUE(services.config_status.has_value());
    MainWindow window{services.services()};
    constructor_driver.stop();

    EXPECT_NE(window.ui->actionToggleRealtime, nullptr);
    EXPECT_EQ(window.ui->menubar->findChildren<QMenu *>().size(), 7);
}
```

Add the includes the new code needs (`<fstream>`, `<iterator>`, `<QSet>`, `<QKeySequence>`, `<QWidgetAction>`, `"src/ui/desktop/menu/testing/menu_snapshot.h"`), the `MAIN_MENU_GOLDEN_PATH` env, golden `data`, and the snapshot dep to `test_mainwindow` in `src/ui/desktop/widgets/BUILD.bazel`:

```starlark
    data = ["testdata/main_menu.golden"],
    env = {
        "MAIN_MENU_GOLDEN_PATH": "$(location testdata/main_menu.golden)",
        "QT_QPA_PLATFORM": "offscreen",
    },
```
(merge into the existing `env`) and `"//src/ui/desktop/menu/testing:menu_snapshot",` in `deps`. Create the golden as a copy of the legacy one for now: `mkdir -p src/ui/desktop/widgets/testdata && cp src/ui/desktop/menu/testdata/legacy_menu.golden src/ui/desktop/widgets/testdata/main_menu.golden`

Switch the test helpers (Task 2) to the static names:

```cpp
QAction *menuAction(MainWindow& window, const ActionName& name)
{
    return window.findChild<QAction *>(QString::fromLatin1(name.member));
}

bool triggerMenu(MainWindow& window, const ActionName& name)
{
    QAction *action = menuAction(window, name);
    if (action == nullptr)
    {
        return false;
    }
    action->trigger();
    return true;
}
```
and remove `legacy_id` from `ActionName` and its constants.

- [ ] **Step 4: Run to verify the new tests fail**

Run: `bazel test --config=release //src/ui/desktop/widgets:test_mainwindow`
Expected: FAIL. The window still builds its menu from `menu.cfg`, so `findChild("actionToggleRealtime")` returns null, and the compile fails first on `window.ui->actionToggleRealtime` (member exists from the generated header, so it compiles; the failures are the null lookups and the golden).

- [ ] **Step 5: Wire the actions**

In `mainwindow.cpp`, replace the block from `QSignalMapper *mapper = nullptr;` through `connect(mapper, SIGNAL(mappedString(QString)), this, SLOT(menu_action_triggered(QString)));` (keep the `setSplashScreenProgress("Setting up menus...", 10);` line above it) with:

```cpp
    apply_standard_shortcuts();
    connect_menu_actions();
```
Remove the now-unused includes `src/backend/config/menu_definition.h` and `src/ui/desktop/menu/menu_builder.h`, and `#include <QSignalMapper>` in `mainwindow.h`.

In `mainwindow.h` replace
```cpp
    // menu_actions.c
    void menu_action_triggered(const QString& action);
```
with
```cpp
    // menu_actions.c
    void connect_menu_actions();
    void apply_standard_shortcuts();
    void show_about_dialog();
```

In `menu_actions.cpp`, delete the `#include "src/algorithms/menu/menu_command.h"` line and the whole `MainWindow::menu_action_triggered` function, and add:

```cpp
void MainWindow::connect_menu_actions()
{
    using fastecu::calibration::IncrementStep;
    using fastecu::calibration::InterpolationMode;

    // Lambdas, not member pointers: QAction::triggered carries a bool that
    // these handlers do not take.
    connect(ui->actionOpenCalibration, &QAction::triggered, this, [this] { open_calibration_file(nullptr); });
    connect(ui->actionSaveCalibration, &QAction::triggered, this, [this] { save_calibration_file(); });
    connect(ui->actionSaveCalibrationAs, &QAction::triggered, this, [this] { save_calibration_file_as(); });
    connect(ui->actionCloseCalibration, &QAction::triggered, this, [this] { close_calibration(); });
    connect(ui->actionQuit, &QAction::triggered, this, [this] { close_app(); });
    connect(ui->actionCopy, &QAction::triggered, this, [this] { copy_value(); });
    connect(ui->actionPaste, &QAction::triggered, this, [this] { paste_value(); });
    connect(ui->actionSettings, &QAction::triggered, this, [this] { show_preferences_window(); });
    connect(ui->actionCoarseIncrement, &QAction::triggered, this, [this] { inc_dec_value(IncrementStep::CoarseUp); });
    connect(ui->actionCoarseDecrement, &QAction::triggered, this, [this] { inc_dec_value(IncrementStep::CoarseDown); });
    connect(ui->actionFineIncrement, &QAction::triggered, this, [this] { inc_dec_value(IncrementStep::FineUp); });
    connect(ui->actionFineDecrement, &QAction::triggered, this, [this] { inc_dec_value(IncrementStep::FineDown); });
    connect(ui->actionSetValue, &QAction::triggered, this, [this] { set_value(); });
    connect(ui->actionInterpolateHorizontal, &QAction::triggered, this,
            [this] { interpolate_value(InterpolationMode::Horizontal); });
    connect(ui->actionInterpolateVertical, &QAction::triggered, this,
            [this] { interpolate_value(InterpolationMode::Vertical); });
    connect(ui->actionInterpolateBidirectional, &QAction::triggered, this,
            [this] { interpolate_value(InterpolationMode::Bidirectional); });
    connect(ui->actionConnectToEcu, &QAction::triggered, this, [this] { connect_to_ecu(); });
    connect(ui->actionDisconnectFromEcu, &QAction::triggered, this, [this] { disconnect_from_ecu(); });
    connect(ui->actionToggleRealtime, &QAction::triggered, this, [this] { toggle_realtime(); });
    connect(ui->actionLogToFile, &QAction::triggered, this, [this] { toggle_log_to_file(); });
    connect(ui->actionReadRomFromEcu, &QAction::triggered, this, [this] { start_ecu_operations("read"); });
    connect(ui->actionTestWriteRomToEcu, &QAction::triggered, this, [this] { start_ecu_operations("test_write"); });
    connect(ui->actionWriteRomToEcu, &QAction::triggered, this, [this] { start_ecu_operations("write"); });
    connect(ui->actionSetLogViews, &QAction::triggered, this, [this] { change_gauge_values(); });
    connect(ui->actionDtcWindow, &QAction::triggered, this, [this] { show_dtc_window(); });
    connect(ui->actionHexEditor, &QAction::triggered, this, [this] { show_hex_editor(); });
    connect(ui->actionTerminal, &QAction::triggered, this, [this] { show_terminal_window(); });
    connect(ui->actionBiuCommunication, &QAction::triggered, this, [this] { show_subaru_biu_window(); });
    connect(ui->actionGetKey, &QAction::triggered, this, [this] { show_subaru_get_key_window(); });
    connect(ui->actionWinolsCsvToRomRaiderXml, &QAction::triggered, this, [this] { winols_csv_to_romraider_xml(); });
    connect(ui->actionAbout, &QAction::triggered, this, [this] { show_about_dialog(); });
}

namespace
{
// Qt's primary binding for a standard key, or `fallback` where the platform
// has none (Save As and Quit on Windows). Designer stores only literal
// sequences, so these are set here rather than in the .ui.
void set_standard_shortcut(QAction *action, QKeySequence::StandardKey key, const char *fallback)
{
    const QKeySequence standard{key};
    action->setShortcut(standard.isEmpty() ? QKeySequence{QString::fromLatin1(fallback)} : standard);
}
} // namespace

void MainWindow::apply_standard_shortcuts()
{
    set_standard_shortcut(ui->actionOpenCalibration, QKeySequence::Open, "Ctrl+O");
    set_standard_shortcut(ui->actionSaveCalibration, QKeySequence::Save, "Ctrl+S");
    set_standard_shortcut(ui->actionSaveCalibrationAs, QKeySequence::SaveAs, "Ctrl+Shift+S");
    set_standard_shortcut(ui->actionCopy, QKeySequence::Copy, "Ctrl+C");
    set_standard_shortcut(ui->actionPaste, QKeySequence::Paste, "Ctrl+V");
    set_standard_shortcut(ui->actionQuit, QKeySequence::Quit, "Ctrl+Q");
}

void MainWindow::show_about_dialog()
{
    QMessageBox::information(this, tr("FastECU"),
                             "FastECU is open source tuning software for Subaru ECUs,\n"
                             "TCUs and also modifying BIU and ECUs of other car makes.\n"
                             "\n"
                             "This is beta test version for read and write ROMs via\n"
                             "K-Line and CAN connection with Open Port 2.0 or generic\n"
                             "OBD2 cable. Software is tested in Win7/Win10 32/64bit\n"
                             "and Linux amd64 and aarch64 platforms.\n"
                             "\n"
                             "There WILL be bugs and things that don't work. Be patient\n"
                             "with new versions relesed.\n"
                             "\n"
                             "All liability lies with the user. We are not responsible any\n"
                             "harm, laws broken or bricked ECUs that can follow for using\n"
                             "this software.\n"
                             "\n"
                             "\n"
                             "Huge thanks to following:\n"
                             "\n"
                             "fenugrec - author of nisprog software\n"
                             "rimwall - modifier of nisprog kernels for Subaru use\n"
                             "SergArb - testing and software development\n"
                             "alesv - testing and software development\n"
                             "jimihimisimi - testing and software development\n"
                             "\n"
                             "...and to all of you who had support software development by\n"
                             "donating! All, even the smallest amount of donates are welcome!\n");
}
```
The text is the `MenuCommand::About` case's, byte for byte; delete that case with the rest of `menu_action_triggered`.

- [ ] **Step 6: Replace the six text-scan sites**

In `menu_actions.cpp`:

```cpp
void MainWindow::set_identification_in_progress(bool in_progress)
{
    log_transport_list->setEnabled(!in_progress);
    ecu_radio_button->setEnabled(!in_progress);
    tcu_radio_button->setEnabled(!in_progress);
    ui->actionConnectToEcu->setEnabled(!in_progress);
    ui->actionToggleRealtime->setEnabled(!in_progress);
}

void MainWindow::set_realtime_state(bool state)
{
    ui->actionToggleRealtime->setChecked(state);
}
```
In `toggle_realtime`, delete the menu scan (`QAction *logger{}; QList<QMenu *> menus ... }`) and begin with `logging_state = ui->actionToggleRealtime->isChecked();` Anywhere later in that function that used `logger`, use `ui->actionToggleRealtime`. In `toggle_log_to_file`, replace the scan with `write_datalog_to_file = ui->actionLogToFile->isChecked();`.

In `mainwindow.cpp`:

```cpp
void MainWindow::set_flash_arrow_state()
{
    ui->actionReadRomFromEcu->setEnabled(static_cast<bool>(protocol_capability(selected_vehicle(), &ProtocolEntry::read)));
    ui->actionTestWriteRomToEcu->setEnabled(
        static_cast<bool>(protocol_capability(selected_vehicle(), &ProtocolEntry::test_write)));
    ui->actionWriteRomToEcu->setEnabled(static_cast<bool>(protocol_capability(selected_vehicle(), &ProtocolEntry::write)));
}
```
and in `restoreLoggingUiState` replace the scan with `ui->actionToggleRealtime->setChecked(false);` (keep the three lines above it).

- [ ] **Step 7: Build and update the golden**

Run: `bazel test --config=release //src/ui/desktop/widgets:test_mainwindow`
Expected: everything passes except `menuMatchesTheGolden`, which prints `actual snapshot:`. The only differences from the legacy golden must be exactly:

1. Every tooltip: the legacy `Name\n\n<tip>` becomes `<tip>` (or the action text when the tip was empty).
2. `Diagnostic Trouble Codes` goes from `icon=0` to `icon=1`.

Overwrite `src/ui/desktop/widgets/testdata/main_menu.golden` with the printed snapshot, then verify with `diff src/ui/desktop/menu/testdata/legacy_menu.golden src/ui/desktop/widgets/testdata/main_menu.golden` that the diff contains only those two kinds of change. Menus, item order, shortcuts, checkable flags and the toolbar must be identical. Anything else is a bug in the `.ui`; fix the `.ui`, not the golden.

- [ ] **Step 8: Run to verify everything passes**

Run: `bazel test --config=release //src/ui/desktop/widgets:test_mainwindow //src/ui/desktop/menu:all`
Expected: PASS (the legacy golden test still passes; the old builder still exists until Task 4).

- [ ] **Step 9: Gazelle and commit**

```bash
python3 scripts/gazelle_check.py --fix
git add src/ui/desktop
git commit -m "refactor: declare the menu in mainwindow.ui and connect actions directly

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FgiHcLMaxFbqt4VFC1xFeN"
```

---

### Task 4: Remove the runtime menu machinery

**Files:**
- Delete: `src/ui/desktop/menu/menu_builder.{h,cpp}`, `menu_builder_test.cpp`, `legacy_menu_golden_test.cpp`, `testdata/legacy_menu.golden`, `src/ui/desktop/menu/BUILD.bazel`
- Delete: `src/backend/config/menu_definition.{h,cpp}`, `menu_definition_test.cpp`
- Delete: `src/algorithms/menu/` (all)
- Delete: `resources/shared/config/menu.cfg`
- Modify: `src/backend/config/BUILD.bazel`, `config_paths.{h,cpp}`, `config_paths_test.cpp`, `resources/shared/BUILD.bazel`, `resources/shared/config.qrc`, `src/ui/desktop/widgets/BUILD.bazel`
- Modify: `docs/design-notes.md`, `docs/tech-debt.md`

**Interfaces:**
- Consumes: Task 3 (nothing may reference the deleted code).

- [ ] **Step 1: Prove nothing live references the machinery**

Run: `grep -rn "menu_definition\|build_menus\|menu_builder\|MenuCommand\|menu_command\|menu_file\|config/menu.cfg" src apps tests scripts resources --include='*.cpp' --include='*.h' --include='*.bazel' --include='*.qrc' --include='*.py' --include='*.sh'`
Expected: hits only in the files this task deletes or edits, plus `menu_snapshot` (which names no deleted symbol). Investigate any other hit before deleting.

- [ ] **Step 2: Delete and edit**

```bash
git rm -r src/ui/desktop/menu/menu_builder.h src/ui/desktop/menu/menu_builder.cpp \
  src/ui/desktop/menu/menu_builder_test.cpp src/ui/desktop/menu/legacy_menu_golden_test.cpp \
  src/ui/desktop/menu/testdata src/ui/desktop/menu/BUILD.bazel \
  src/backend/config/menu_definition.h src/backend/config/menu_definition.cpp \
  src/backend/config/menu_definition_test.cpp src/algorithms/menu \
  resources/shared/config/menu.cfg
```
- `src/backend/config/BUILD.bazel`: remove the `menu_definition` and `menu_definition_test` targets.
- `config_paths.h`: remove `std::string menu_file;`. `config_paths.cpp`: remove the `paths.menu_file = ...` line. `config_paths_test.cpp`: remove the `menu_file` expectation (line 20).
- `resources/shared/BUILD.bazel`: remove the `"config/menu.cfg",` entry. `resources/shared/config.qrc`: remove `<file alias="config/menu.cfg">config/menu.cfg</file>`.
- `src/ui/desktop/widgets/BUILD.bazel`: remove `"//src/algorithms/menu",`, `"//src/backend/config:menu_definition",` and `"//src/ui/desktop/menu:menu_builder",` from `implementation_deps`.
- `mainwindow_test.cpp`: remove any remaining `menu.cfg` reference (the fixture's stale-file test writes one on purpose; keep that, it is Review Focus 1).

- [ ] **Step 3: Update docs**

- `docs/design-notes.md` (~line 730): change "none was in the shipped `menu.cfg`" to "none was in the shipped menu".
- `docs/tech-debt.md`: in "P1: Separate UI from application logic", delete "menu actions found by their text" from the widget-selection risk bullet, and delete the whole "Minor code-level findings" bullet beginning "`MainWindow::restoreLoggingUiState()` ... finds the menu action whose text is `Logging`". Keep the remaining actions and risks as they are.

- [ ] **Step 4: Verify the whole graph**

```bash
python3 scripts/gazelle_check.py --fix
bazel test --config=release //...
prek run --all-files
```
Expected: all pass. `bazel test //...` includes the Gazelle and layering guards; a failure there is a real violation, not noise.

- [ ] **Step 5: Check no new ratchet entry slipped in**

Run: `git diff master -- bazel/qt/BUILD.bazel`
Expected: no added lines.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor: remove the runtime menu builder, parser, MenuCommand and menu.cfg

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FgiHcLMaxFbqt4VFC1xFeN"
```

---

## Verification before the PR

- `bazel test --config=release //...` and `prek run --all-files` pass.
- Bench is not required: no ECU I/O path changed. The first manual check on a real build is that the menu bar, toolbar order, Space/F3/F4 shortcuts and the Logging toggle behave as before; record that in the PR description.
- Open the PR from `refactor/static-menu` (FastECU uses pull requests; do not commit to `master`).
