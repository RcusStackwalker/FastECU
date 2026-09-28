# Step 6m-3: Calibration Tree, ROM Info and View State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the calibration files and data trees, and the ROM Info rows, from the session instead of the legacy struct. Move the per-ROM view state into the UI. Make `RomProtocolInfo` the one source of ROM metadata.

**Architecture:**
- **ROM info rows.** A UI helper, `rom_info_values(session, placeholder_make)`, produces the 16 ROM Info strings exactly as legacy showed them. It is equivalence-tested against the 6m-2 projection, which is itself golden against legacy. The projection is refactored to use the helper, and `refresh_legacy_metadata` copies session metadata one-way into the legacy view for the map-window and flash readers that remain until 6m-4.
- **View state.** `CalibrationViewState` (open map windows, expanded categories, ROM Info expanded, continue-without make) lives beside each session in `MainWindow`.
- **Trees.** `CalibrationTreeWidget` builds both trees from the session plus that state.
- **Unchanged here.** Map windows, edits, hex, save, write bytes and checksum stay on the legacy view (6m-4).

**Tech Stack:** C++23, Bazel, Qt 6 widgets, QtTest (`fastecu_qttest`), GoogleTest (`fastecu_gtest`), prek, `gh stack`.

**Spec:** [Step 6m — Definition and calibration sessions](../specs/2026-09-28-step6m-calibration-sessions-design.md), section "6m-3 — Tree, ROM info and view state".

## Global Constraints

- **Tree output is unchanged:** item texts, icons, check states, tooltips, expansion, the `LOG_D` "Set <label>: <value>" lines, and column 1/2 contents. Legacy `legacy_value` placeholders (`" "` for an empty string) are part of the output, so a map whose name or category is empty is not listed.
- **Metadata ownership.**
  - After this slice, ROM metadata (flash method, checksum module, MCU, kernel path/address, ROM ID, file size) is written only through `CalibrationSession::set_protocol`.
  - The legacy view's copies are refreshed from the session by `refresh_legacy_metadata`. Nothing writes them directly.
  - Bytes stay in the legacy view (6m-2 rule, unchanged).
- The write path fills the flash method only when legacy did: the ROM Info flash-method value is exactly `""`, which happens only when a definition left it empty.
- No new `ErrorKind`. No `qt_layer` entry. Results are checked with `.has_value()`. Synthetic data only. Markdown cross-references are links.
- No wire behavior, flash sequence, definition schema or ROM format change. No bench checklist change.
- Branch `feat/6m-3-calibration-tree`, one PR onto `master` (`gh stack init --base master feat/6m-3-calibration-tree`, `gh stack submit --auto --open`) after the user authorizes the push.
- Gates: `bazel build -k --config=release //...`, `bazel test -k --config=release //...`, `prek run --all-files`, and `bazel run --config=release //:clang_tidy_report_changed`. Platform CI is required, and anything not run locally is reported as pending.
- Every commit message ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

## Review Focus

- **Expanding or collapsing a category, then switching to another ROM and back.** Expected: each ROM keeps its own expansion and open-map checkmarks. Pinned by `MainWindowTest::viewStateIsKeptPerRom` (Task 5).
- **Closing a ROM while the data tree shows a different ROM** (selection moved by keyboard). Expected: every map window of the closed ROM closes; other ROMs' windows stay. Pinned by `MainWindowTest::closingARomClosesAllOfItsWindows` (Task 5).
- **A definition whose `<flashmethod>` is empty, then Write to ECU.** Expected: the selected protocol is filled in, the vehicle is re-selected, and the ROM Info row updates. A definition-less ROM (value `" "`) is not filled. Pinned by `RomInfo.EmptyDefinitionFlashMethodIsTheWriteFillCondition` (Task 1) and `LegacyCalibrationView.RefreshCopiesSessionMetadata` (Task 2); the MainWindow wiring is covered by review.
- **Continue without definition.** Expected: ROM Info shows `UnknownID`, empty IDs, the selected vehicle's make and a blank definition file, on that ROM only. Pinned by `RomInfo.ContinueWithoutPlaceholdersMatchLegacy` (Task 1) and `MainWindowTest::definitionlessOpenPromptsOnceAndAppliesPlaceholders` (Task 5).
- **A map whose category or name is empty, a 1×1 "3D" map, and a map with no description.** Expected: the first is not listed, the 1×1 map gets the 1D icon, and the tooltip is the name plus a space. Pinned by `CalibrationTreeWidgetTest::dataTreeMatchesLegacyRules` (Task 4).

---

## File Structure

| File | Change | Task |
|---|---|---|
| Spec (5-slice split) and this plan | Commit | 0 |
| `src/ui/desktop/calibration/rom_info.{h,cpp}`, BUILD | Create | 1 |
| `src/ui/desktop/calibration/legacy_calibration_view_test.cpp` | ROM-info equivalence tests | 1, 2 |
| `src/ui/desktop/calibration/legacy_calibration_view.{h,cpp}` | Use `rom_info_values`; add `refresh_legacy_metadata` | 2 |
| `src/ui/desktop/definition/definition_authoring_dialog.{h,cpp}`, `src/backend/definitions/file_actions.{h,cpp}`, `model_validation_test.cpp` | Header constants move; dialog takes no struct | 3 |
| `src/ui/desktop/calibration/calibration_view_state.h`, BUILD | Create | 4 |
| `src/ui/desktop/calibration_treewidget.{h,cpp}`, `calibration_treewidget_test.cpp`, `src/ui/desktop/BUILD.bazel` | Build trees from the session | 4 |
| `src/ui/desktop/mainwindow.{h,cpp}`, `mainwindow_test.cpp` | View state, tree handlers, metadata writes, window cleanup | 5 |
| `docs/modularization-plan.md` | Progress | 6 |

---

### Task 0: Commit the spec split and this plan

- [ ] **Step 1: Commit** (the spec was edited on this branch before the plan was written)

```bash
git add docs/superpowers
git commit -m "docs: split step 6m into five slices and plan 6m-3

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 1: `rom_info_values`

**Files:**
- Create: `src/ui/desktop/calibration/rom_info.h`, `rom_info.cpp`
- Modify: `src/ui/desktop/calibration/BUILD.bazel`, `legacy_calibration_view_test.cpp`

**Interfaces:**
- Produces, in namespace `fastecu::ui`:

```cpp
enum class RomInfoRow : int
{
    XmlId, InternalIdAddress, InternalIdString, EcuId, Make, Market, Model, SubModel,
    Transmission, Year, FlashMethod, MemModel, ChecksumModule, RomBase, FileSize, DefFile,
};
inline constexpr int kRomInfoRowCount = 16;
QStringList rom_info_labels();                 // "XML ID", ..., "Def File"
QString rom_info_value(const QStringList& values, RomInfoRow row);
// The 16 ROM Info values legacy displayed for this session. A set
// placeholder_make applies the "continue without definition" placeholders.
QStringList rom_info_values(const calibration::CalibrationSession& session,
                            const std::optional<QString>& placeholder_make = std::nullopt);
```

- [ ] **Step 1: Write the failing tests**

Append to `legacy_calibration_view_test.cpp` (same fixture), inside its namespace, and add `#include "src/ui/desktop/calibration/rom_info.h"`:

```cpp
TEST_F(LegacyCalibrationView, RomInfoLabelsAndRowsMatchLegacyIndices)
{
    const FileActions::EcuCalDefStructure legacy;
    EXPECT_EQ(rom_info_labels(), legacy.RomInfoStrings);
    EXPECT_EQ(static_cast<int>(RomInfoRow::FlashMethod), FileActions::FlashMethod);
    EXPECT_EQ(static_cast<int>(RomInfoRow::DefFile), FileActions::DefFile);
    EXPECT_EQ(kRomInfoRowCount, legacy.RomInfoStrings.size());
}

TEST_F(LegacyCalibrationView, RomInfoMatchesTheLegacyViewForEveryOpenKind)
{
    cfg.file_repository.files["/cal/a.bin"] = synthetic_rom();
    const auto plain = opener.open_file("/cal/a.bin");
    ASSERT_THAT(plain, IsOk());
    const calibration::CalibrationSession plain_session(calibration::SessionId{1}, plain->contents);
    EXPECT_EQ(rom_info_values(plain_session), projected(*plain).RomInfo);

    enable_ecuflash_definitions();
    ASSERT_THAT(cfg.session.select_row(0), IsOk());
    const auto defined = opener.open_file("/cal/a.bin");
    ASSERT_THAT(defined, IsOk());
    ASSERT_TRUE(defined->contents.definition.has_value());
    const calibration::CalibrationSession defined_session(calibration::SessionId{2}, defined->contents);
    EXPECT_EQ(rom_info_values(defined_session), projected(*defined).RomInfo);

    ASSERT_THAT(cfg.session.select_row(0), IsOk());
    const auto read = opener.adopt_read_image(calibration::ReadImage{
        .rom = synthetic_rom(), .filename = "r.bin", .rom_id = "X", .protocol_name = "proto_b"});
    ASSERT_THAT(read, IsOk());
    const calibration::CalibrationSession read_session(calibration::SessionId{3}, read->contents);
    EXPECT_EQ(rom_info_values(read_session), projected(*read).RomInfo);
}

TEST_F(LegacyCalibrationView, ContinueWithoutPlaceholdersMatchLegacy)
{
    cfg.file_repository.files["/cal/a.bin"] = synthetic_rom();
    const auto outcome = opener.open_file("/cal/a.bin");
    ASSERT_THAT(outcome, IsOk());
    const calibration::CalibrationSession session(calibration::SessionId{1}, outcome->contents);
    FileActions::EcuCalDefStructure legacy = projected(*outcome);

    file_actions.apply_missing_definition_defaults(&legacy); // make from the selected vehicle

    EXPECT_EQ(rom_info_values(session, QString::fromStdString(cfg.session.selected_vehicle()->make)),
              legacy.RomInfo);
    EXPECT_EQ(rom_info_value(legacy.RomInfo, RomInfoRow::XmlId), QString("UnknownID"));
}

TEST_F(LegacyCalibrationView, EmptyDefinitionFlashMethodIsTheWriteFillCondition)
{
    // Legacy filled the flash method on write only when RomInfo held "" --
    // a definition with no <flashmethod>. A definition-less ROM holds " ".
    enable_ecuflash_definitions();
    std::string text{kDefinition};
    text.erase(text.find("<flashmethod>alias_a</flashmethod>"), std::string_view("<flashmethod>alias_a</flashmethod>").size());
    cfg.put("/defs/test.xml", text);
    cfg.file_repository.files["/cal/a.bin"] = synthetic_rom();
    const auto defined = opener.open_file("/cal/a.bin");
    ASSERT_THAT(defined, IsOk());
    const calibration::CalibrationSession defined_session(calibration::SessionId{1}, defined->contents);
    EXPECT_EQ(rom_info_value(rom_info_values(defined_session), RomInfoRow::FlashMethod), QString(""));

    cfg.session.settings().use_ecuflash_definitions = "disabled";
    const auto plain = opener.open_file("/cal/a.bin");
    ASSERT_THAT(plain, IsOk());
    const calibration::CalibrationSession plain_session(calibration::SessionId{2}, plain->contents);
    EXPECT_EQ(rom_info_value(rom_info_values(plain_session), RomInfoRow::FlashMethod), QString(" "));
}
```

Add to `src/ui/desktop/calibration/BUILD.bazel`:

```python
qt_cc_library(
    name = "rom_info",
    srcs = ["rom_info.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["rom_info.h"],
    deps = QT_DEPS + ["//src/backend/calibration/session:calibration_session"],
)
```

Then add `":rom_info"` to `legacy_calibration_view_test`'s `deps`.

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/ui/desktop/calibration:legacy_calibration_view_test`
Expected: FAIL to build, because `rom_info.h` does not exist.

- [ ] **Step 3: Implement**

`src/ui/desktop/calibration/rom_info.h`:

```cpp
#pragma once

#include <optional>

#include <QString>
#include <QStringList>

#include "src/backend/calibration/session/calibration_session.h"

namespace fastecu::ui
{

// The "ROM Info" rows of the calibration data tree, in legacy order
// (EcuCalDefStructure::RomInfoStrings / FileActions::RomInfoEnum).
enum class RomInfoRow : int
{
    XmlId,
    InternalIdAddress,
    InternalIdString,
    EcuId,
    Make,
    Market,
    Model,
    SubModel,
    Transmission,
    Year,
    FlashMethod,
    MemModel,
    ChecksumModule,
    RomBase,
    FileSize,
    DefFile,
};
inline constexpr int kRomInfoRowCount = 16;

QStringList rom_info_labels();
QString rom_info_value(const QStringList& values, RomInfoRow row);

// The 16 ROM Info values legacy displayed: MainWindow's " " pre-fill, the
// definition's identity and metadata (FileActions::open_subaru_rom_file after
// normalize_definition_addresses), then the session's protocol info. A set
// placeholder_make applies FileActions::apply_missing_definition_defaults.
QStringList rom_info_values(const calibration::CalibrationSession& session,
                            const std::optional<QString>& placeholder_make = std::nullopt);

} // namespace fastecu::ui
```

`src/ui/desktop/calibration/rom_info.cpp`:

```cpp
#include "src/ui/desktop/calibration/rom_info.h"

#include <format>

namespace fastecu::ui
{
namespace
{

QString qs(const std::string& text)
{
    return QString::fromStdString(text);
}

void set(QStringList& values, RomInfoRow row, const QString& value)
{
    values[static_cast<int>(row)] = value;
}

} // namespace

QStringList rom_info_labels()
{
    return {
        "XML ID", "Internal ID Address", "Internal ID String", "ECU ID", "Make",           "Market",
        "Model",  "Submodel",            "Transmission",       "Year",   "Flash Method",   "Memory Model",
        "Checksum Module", "Rom Base",   "File Size",          "Def File",
    };
}

QString rom_info_value(const QStringList& values, RomInfoRow row)
{
    return values.value(static_cast<int>(row));
}

QStringList rom_info_values(const calibration::CalibrationSession& session,
                            const std::optional<QString>& placeholder_make)
{
    const calibration::RomProtocolInfo& protocol = session.protocol();
    QStringList values(kRomInfoRowCount, QString(" "));
    if (const calibration::ResolvedDefinition *resolved = session.definition(); resolved != nullptr)
    {
        // populate_rom_info starts from empty strings, not the " " pre-fill.
        values = QStringList(kRomInfoRowCount, QString{});
        const definition::RomDefinition& definition = resolved->definition;
        set(values, RomInfoRow::XmlId, qs(definition.identity.xml_id));
        set(values, RomInfoRow::InternalIdAddress,
            definition.identity.internal_id_address.has_value()
                ? qs(std::format("{:x}", *definition.identity.internal_id_address)) // "0x" stripped
                : QString{});
        set(values, RomInfoRow::InternalIdString, qs(definition.identity.internal_id));
        set(values, RomInfoRow::EcuId, qs(definition.identity.ecu_id));
        set(values, RomInfoRow::Make, qs(definition.metadata.make));
        set(values, RomInfoRow::Market, qs(definition.metadata.market));
        set(values, RomInfoRow::Model, qs(definition.metadata.model));
        set(values, RomInfoRow::SubModel, qs(definition.metadata.submodel));
        set(values, RomInfoRow::Transmission, qs(definition.metadata.transmission));
        set(values, RomInfoRow::Year, qs(definition.metadata.year));
        set(values, RomInfoRow::FlashMethod, qs(protocol.flash_method));
        set(values, RomInfoRow::MemModel, qs(definition.metadata.memory_model));
        set(values, RomInfoRow::ChecksumModule, qs(definition.metadata.checksum_module));
        set(values, RomInfoRow::RomBase, definition.parents.empty() ? QString{} : qs(definition.parents.front()));
        set(values, RomInfoRow::DefFile, qs(definition.source));
    }
    else if (!protocol.flash_method.empty())
    {
        set(values, RomInfoRow::FlashMethod, qs(protocol.flash_method));
    }
    if (!protocol.checksum_module.empty())
    {
        set(values, RomInfoRow::ChecksumModule, qs(protocol.checksum_module));
    }
    set(values, RomInfoRow::FileSize, qs(protocol.file_size_label));
    if (placeholder_make.has_value())
    {
        set(values, RomInfoRow::XmlId, "UnknownID");
        set(values, RomInfoRow::InternalIdAddress, "");
        set(values, RomInfoRow::InternalIdString, "");
        set(values, RomInfoRow::EcuId, "");
        set(values, RomInfoRow::Make, *placeholder_make);
        set(values, RomInfoRow::DefFile, " ");
    }
    return values;
}

} // namespace fastecu::ui
```

- [ ] **Step 4: Run to verify they pass**

Run: `bazel test --config=release //src/ui/desktop/calibration:all`
Expected: PASS. On a mismatch, compare element by element against the 6m-2 projection and fix `rom_info_values`, never the projection.

- [ ] **Step 5: Mutation check.** Replace `std::format("{:x}", …)` with `std::format("0x{:x}", …)` temporarily. `RomInfoMatchesTheLegacyViewForEveryOpenKind` must fail. Revert.

- [ ] **Step 6: Commit**

```bash
git add src/ui/desktop/calibration
git commit -m "feat(ui): derive ROM info rows from a calibration session (6m-3)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 2: The projection uses `rom_info_values`; add `refresh_legacy_metadata`

**Files:**
- Modify: `src/ui/desktop/calibration/legacy_calibration_view.{h,cpp}`, `legacy_calibration_view_test.cpp`, `BUILD.bazel` (`legacy_calibration_view` deps `+ ":rom_info"`)

**Interfaces:**
- Produces: `void refresh_legacy_metadata(const calibration::CalibrationSession& session, FileActions::EcuCalDefStructure& legacy);` It copies the session's metadata into the legacy view: `RomInfo[FlashMethod]`, `RomInfo[ChecksumModule]` and `RomInfo[FileSize]` (from `rom_info_values`), plus `RomId`, `McuType`, `Kernel` and `KernelStartAddr`. It touches nothing else.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(LegacyCalibrationView, RefreshCopiesSessionMetadata)
{
    cfg.file_repository.files["/cal/a.bin"] = synthetic_rom();
    const auto outcome = opener.open_file("/cal/a.bin");
    ASSERT_THAT(outcome, IsOk());
    calibration::CalibrationSession session(calibration::SessionId{1}, outcome->contents);
    FileActions::EcuCalDefStructure legacy = projected(*outcome);
    legacy.MapData = QStringList{"untouched"};

    calibration::RomProtocolInfo protocol = session.protocol();
    protocol.flash_method = "proto_b";
    protocol.kernel_path = "/k/b.bin";
    protocol.kernel_start_address = "0x0";
    protocol.mcu_type = "M32R";
    protocol.rom_id = "RID";
    session.set_protocol(protocol);

    refresh_legacy_metadata(session, legacy);

    EXPECT_EQ(legacy.RomInfo.at(FileActions::FlashMethod), QString("proto_b"));
    EXPECT_EQ(legacy.Kernel, QString("/k/b.bin"));
    EXPECT_EQ(legacy.KernelStartAddr, QString("0x0"));
    EXPECT_EQ(legacy.McuType, QString("M32R"));
    EXPECT_EQ(legacy.RomId, QString("RID"));
    EXPECT_EQ(legacy.MapData, QStringList{"untouched"});
}
```

- [ ] **Step 2: Run to verify it fails** (`bazel test --config=release //src/ui/desktop/calibration:legacy_calibration_view_test`). Expected: FAIL to compile, because `refresh_legacy_metadata` is undeclared.

- [ ] **Step 3: Implement**

In the header, declare it below `project_legacy_calibration`, with the comment: "One-way copy of the session's ROM metadata into a legacy view (6m-3). The session is the only writer; readers that still take the legacy struct see the refreshed values."

In the `.cpp`, include `rom_info.h`, add the function, and make the projection call it. The projection's metadata assignments move into it:

```cpp
void refresh_legacy_metadata(const calibration::CalibrationSession& session, FileActions::EcuCalDefStructure& legacy)
{
    const QStringList values = rom_info_values(session);
    for (const RomInfoRow row : {RomInfoRow::FlashMethod, RomInfoRow::ChecksumModule, RomInfoRow::FileSize})
    {
        legacy.RomInfo[static_cast<int>(row)] = rom_info_value(values, row);
    }
    const calibration::RomProtocolInfo& protocol = session.protocol();
    legacy.RomId = qs(protocol.rom_id);
    legacy.McuType = qs(protocol.mcu_type);
    legacy.Kernel = qs(protocol.kernel_path);
    legacy.KernelStartAddr = qs(protocol.kernel_start_address);
}
```

In `project_legacy_calibration`, make these changes:
1. After the definition branch (`project_definition` + `0x` stripping), set `legacy.RomInfo = rom_info_values(session);`. This replaces the pre-fill and the individual `RomInfo` assignments.
2. Call `refresh_legacy_metadata(session, legacy);` in place of the separate `RomId`/`McuType`/`Kernel`/`KernelStartAddr` assignments.
3. Keep `FileSize`, the `FlashMethod` field (ECU read), `FileName`, `FullFileName`, `FullRomData`, `OemEcuFile` and the map decode as they are.

The `RomInfo` address stripping becomes redundant because `rom_info_values` already strips it; delete that one line and keep the `AddressList`/axis stripping.

- [ ] **Step 4: Run to verify they pass.** Run `bazel test --config=release //src/ui/desktop/calibration:all`. Expected: PASS, including all four 6m-2 equivalence tests unchanged.

- [ ] **Step 5: Commit**

```bash
git add src/ui/desktop/calibration
git commit -m "refactor(ui): one ROM-info derivation for the legacy view and its metadata refresh (6m-3)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3: Definition header constants move to the authoring dialog

**Files:**
- Modify: `src/ui/desktop/definition/definition_authoring_dialog.{h,cpp}`, `src/backend/definitions/file_actions.{h,cpp}` (`collect_ecuflash_base_header_fields`), `src/backend/definitions/model_validation_test.cpp`, `src/ui/desktop/mainwindow.cpp` (two calls)

**Interfaces:**
- `static QStringList FileActions::collect_ecuflash_base_header_fields(const QStringList& header_names, const QStringList& defData, int *endIndex = nullptr);` The field names are now passed in.
- `bool DefinitionAuthoringDialog::create_new_definition();` and `bool use_existing_definition();` take no struct.
- In `definition_authoring_dialog.h`: `QStringList definition_header_labels();` and `QStringList definition_header_names();`, which return legacy `DefHeaderStrings` / `DefHeaderNames` verbatim.

- [ ] **Step 1: Write the failing test.** Add to `src/ui/desktop/definition/definition_authoring_dialog_test.cpp` (it already builds against the dialog; follow its existing test style):

```cpp
TEST(DefinitionHeaderFields, MatchTheLegacyModelVerbatim)
{
    const FileActions::EcuCalDefStructure legacy;
    EXPECT_EQ(fastecu::ui::definition_header_labels(), legacy.DefHeaderStrings);
    EXPECT_EQ(fastecu::ui::definition_header_names(), legacy.DefHeaderNames);
}
```

  If that file is a QtTest suite rather than gtest, write it as a private slot with `QCOMPARE`. Match the file.

  In `model_validation_test.cpp`, change the call to `FileActions::collect_ecuflash_base_header_fields(ecuCalDef.DefHeaderNames, xmlLines, &endIndex)`.

- [ ] **Step 2: Run to verify it fails.** Run `bazel test --config=release //src/ui/desktop/definition:all //src/backend/definitions:all`. Expected: FAIL to compile.

- [ ] **Step 3: Implement**
  - **`file_actions.{h,cpp}`:** change the first parameter to `const QStringList& header_names`, and iterate `header_names` where `ecuCalDef.DefHeaderNames` was.
  - **Authoring dialog:** in the namespace the dialog uses, define the two functions, copying the literal lists from `src/backend/definitions/ecu_cal_def.h` (`DefHeaderStrings`, `DefHeaderNames`).
  - **`create_new_definition()`:** calls `run_header_dialog(dialog, definition_header_labels(), definition_header_names(), {})`.
  - **`use_existing_definition()`:** calls `collect_ecuflash_base_header_fields(definition_header_names(), {…})` and `run_header_dialog(dialog, definition_header_labels(), names, values)`.
  - **`mainwindow.cpp`:** `definitionAuthoringDialog->create_new_definition();` and `->use_existing_definition();`.
  - Update the header comment that mentions `ecuCalDef`.

- [ ] **Step 4: Run to verify.** Run `bazel test --config=release //src/ui/desktop/definition:all //src/backend/definitions:all` and `bazel build --config=release //src/ui/desktop:desktop`. Expected: PASS / builds.

- [ ] **Step 5: Commit**

```bash
git add src/ui/desktop/definition src/backend/definitions src/ui/desktop/mainwindow.cpp
git commit -m "refactor(ui): keep definition header fields in the authoring dialog (6m-3)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 4: Trees built from the session

**Files:**
- Create: `src/ui/desktop/calibration/calibration_view_state.h`, `src/ui/desktop/calibration_treewidget_test.cpp`
- Modify: `src/ui/desktop/calibration/BUILD.bazel`, `src/ui/desktop/calibration_treewidget.{h,cpp}`, `src/ui/desktop/BUILD.bazel`

**Interfaces:**
- Produces `fastecu::ui::CalibrationViewState` (header-only; target `calibration_view_state`):

```cpp
struct CalibrationViewState
{
    std::set<std::size_t> open_maps;        // legacy VisibleList "1"
    std::set<QString> expanded_categories;  // legacy CategoryExpandedList "1", by category name
    bool rom_info_expanded{false};          // legacy RomInfoExpanded "1"
    std::optional<QString> missing_definition_make; // continue-without placeholders
};
```

- `CalibrationTreeWidget`:
  - `QTreeWidget *buildCalibrationFilesTree(fastecu::calibration::SessionId, QTreeWidget *, const fastecu::calibration::CalibrationSession&);`
  - `QTreeWidget *buildCalibrationDataTree(QTreeWidget *, const fastecu::calibration::CalibrationSession&, const fastecu::ui::CalibrationViewState&);`
  - The two `...ItemExpanded/Collapsed` methods are **removed**; `MainWindow` owns the state (Task 5).

- [ ] **Step 1: Write the failing test**

Create `src/ui/desktop/calibration_treewidget_test.cpp`:

```cpp
#include <QIcon>
#include <QTest>
#include <QTreeWidget>

#include "src/ui/desktop/calibration/session_key.h"
#include "src/ui/desktop/calibration_treewidget.h"

namespace
{

using fastecu::calibration::CalibrationSession;
using fastecu::calibration::SessionContents;
using fastecu::calibration::SessionId;

fastecu::definition::CalibrationMap map(std::string name, std::string category, std::string type,
                                         std::uint32_t x, std::uint32_t y, std::string description = {},
                                         std::string id = {})
{
    fastecu::definition::CalibrationMap m;
    m.id = std::move(id);
    m.name = std::move(name);
    m.category = std::move(category);
    m.type = std::move(type);
    m.x_size = x;
    m.y_size = y;
    m.description = std::move(description);
    return m;
}

CalibrationSession session_with_maps()
{
    fastecu::definition::RomDefinition definition{.format = fastecu::definition::DefinitionFormat::EcuFlash};
    definition.identity.xml_id = "TREE";
    definition.maps = {
        map("Idle", "Idle", "1D", 1, 1, "Idle speed", "idle-id"), // 0
        map("Fuel", "Fuel", "2D", 4, 1),                           // 1: no description
        map("Timing", "Fuel", "3D", 4, 2),                         // 2
        map("Mode", "Switches", "Selectable", 1, 1),               // 3
        map("Hidden", "", "1D", 1, 1),                             // 4: no category -> not listed
        map("", "Fuel", "1D", 1, 1),                               // 5: no name -> not listed
        map("Tiny", "Fuel", "3D", 1, 1),                           // 6: 1x1 -> 1D icon
    };
    return CalibrationSession(SessionId{7}, SessionContents{
                                                .source = {.display_name = "t.bin", .path = "/t.bin"},
                                                .rom = std::vector<std::uint8_t>(16, 0),
                                                .definition = fastecu::calibration::ResolvedDefinition{
                                                    .id = "TREE", .definition = definition},
                                                .protocol = {.file_size_label = "0kb"},
                                            });
}

bool same_icon(const QTreeWidgetItem *item, const char *path)
{
    return item->icon(0).pixmap(16).toImage() == QIcon(path).pixmap(16).toImage();
}

} // namespace

class CalibrationTreeWidgetTest : public QObject
{
    Q_OBJECT

  private slots:
    void filesTreeCarriesNameFirstMapIdAndSessionKey()
    {
        QTreeWidget files;
        CalibrationTreeWidget builder;
        const CalibrationSession session = session_with_maps();

        builder.buildCalibrationFilesTree(session.id(), &files, session);

        QCOMPARE(files.topLevelItemCount(), 1);
        QTreeWidgetItem *item = files.topLevelItem(0);
        QCOMPARE(item->text(0), QString("t.bin"));
        QCOMPARE(item->text(1), QString("idle-id"));
        QCOMPARE(item->text(2), fastecu::ui::session_key_text(SessionId{7}));
        QCOMPARE(item->checkState(0), Qt::Checked);
        QVERIFY(item->isSelected());
    }

    void dataTreeMatchesLegacyRules()
    {
        QTreeWidget data;
        CalibrationTreeWidget builder;
        const CalibrationSession session = session_with_maps();
        fastecu::ui::CalibrationViewState view;
        view.open_maps = {2};
        view.expanded_categories = {"Fuel"};
        view.rom_info_expanded = true;

        builder.buildCalibrationDataTree(&data, session, view);

        QCOMPARE(data.topLevelItemCount(), 4);
        QTreeWidgetItem *rom_info = data.topLevelItem(0);
        QCOMPARE(rom_info->text(0), QString("ROM Info"));
        QVERIFY(rom_info->isExpanded());
        QCOMPARE(rom_info->childCount(), 16);
        QCOMPARE(rom_info->child(0)->text(0), QString("XML ID: TREE"));

        QCOMPARE(data.topLevelItem(1)->text(0), QString("Idle"));
        QVERIFY(!data.topLevelItem(1)->isExpanded());
        QTreeWidgetItem *fuel = data.topLevelItem(2);
        QCOMPARE(fuel->text(0), QString("Fuel"));
        QVERIFY(fuel->isExpanded());
        QCOMPARE(data.topLevelItem(3)->text(0), QString("Switches"));

        QCOMPARE(fuel->childCount(), 3); // Fuel, Timing, Tiny; the nameless map is skipped
        QCOMPARE(fuel->child(0)->text(0), QString("Fuel"));
        QCOMPARE(fuel->child(0)->text(1), QString("1"));
        QCOMPARE(fuel->child(0)->toolTip(0), QString("Fuel ")); // legacy_value(" ") description
        QCOMPARE(fuel->child(0)->checkState(0), Qt::Unchecked);
        QVERIFY(same_icon(fuel->child(0), ":/icons/2D-64.png"));
        QCOMPARE(fuel->child(1)->text(1), QString("2"));
        QCOMPARE(fuel->child(1)->checkState(0), Qt::Checked);
        QVERIFY(same_icon(fuel->child(1), ":/icons/3D-64.png"));
        QCOMPARE(fuel->child(2)->text(1), QString("6"));
        QVERIFY(same_icon(fuel->child(2), ":/icons/1D-64.png"));

        QTreeWidgetItem *idle = data.topLevelItem(1)->child(0);
        QCOMPARE(idle->toolTip(0), QString("IdleIdle speed"));
        QVERIFY(same_icon(idle, ":/icons/1D-64.png"));
        QVERIFY(same_icon(data.topLevelItem(3)->child(0), ":/icons/1D-64.png")); // Selectable
    }

    void definitionlessRomShowsOnlyRomInfo()
    {
        QTreeWidget data;
        CalibrationTreeWidget builder;
        const CalibrationSession session(SessionId{1}, SessionContents{.rom = std::vector<std::uint8_t>(4, 0)});

        builder.buildCalibrationDataTree(&data, session, {});

        QCOMPARE(data.topLevelItemCount(), 1);
        QCOMPARE(data.topLevelItem(0)->text(0), QString("ROM Info"));
        QVERIFY(!data.topLevelItem(0)->isExpanded());
    }
};

QTEST_MAIN(CalibrationTreeWidgetTest)
#include "calibration_treewidget_test.moc"
```

Add the test target to `src/ui/desktop/BUILD.bazel`, modelled on `test_mainwindow`:

```python
fastecu_qttest(
    name = "test_calibration_treewidget",
    size = "small",
    src = "calibration_treewidget_test.cpp",
    copts = ["-DQT_WIDGETS_LIB"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    linkstatic = True,
    deps = [
        ":desktop",
        "//src/ui/desktop/calibration:calibration_view_state",
        "//src/ui/desktop/calibration:session_key",
    ],
)
```

If `fastecu_qttest` names the moc include differently, match `mainwindow_test.cpp`'s trailing `#include`.

Add to `src/ui/desktop/calibration/BUILD.bazel`:

```python
cc_library(
    name = "calibration_view_state",
    hdrs = ["calibration_view_state.h"],
    deps = QT_DEPS,
)
```

Add a `load("@rules_cc//cc:cc_library.bzl", "cc_library")` if the file lacks it, or use `qt_cc_library` with `normal_hdrs` like its siblings if a plain `cc_library` cannot see Qt headers.

- [ ] **Step 2: Run to verify it fails.** Run `bazel test --config=release //src/ui/desktop:test_calibration_treewidget`. Expected: FAIL to build (signatures / missing header).

- [ ] **Step 3: Implement**

`src/ui/desktop/calibration/calibration_view_state.h`:

```cpp
#pragma once

#include <cstddef>
#include <optional>
#include <set>

#include <QString>

namespace fastecu::ui
{

// Presentation state of one open calibration: which map windows are open,
// which data-tree categories are expanded, and whether the operator chose
// "continue without definition" (and with which vehicle make). Legacy kept
// these as VisibleList / CategoryExpandedList / RomInfoExpanded / RomInfo
// placeholders inside EcuCalDefStructure; they are UI state, not calibration
// data.
struct CalibrationViewState
{
    std::set<std::size_t> open_maps;
    std::set<QString> expanded_categories;
    bool rom_info_expanded{false};
    std::optional<QString> missing_definition_make;
};

} // namespace fastecu::ui
```

`calibration_treewidget.h`: include `calibration_session.h`, `calibration/calibration_view_state.h` and `calibration/session_key.h`. Replace the four method declarations with the two new signatures. `file_actions.h` may be dropped if nothing else in the header needs it.

`calibration_treewidget.cpp`: keep `CalibrationTreeWidget::CalibrationTreeWidget()`, and replace the rest with:

```cpp
namespace
{

QString legacy_value(const std::string& value)
{
    return value.empty() ? QString(" ") : QString::fromStdString(value);
}

} // namespace

QTreeWidget *CalibrationTreeWidget::buildCalibrationFilesTree(fastecu::calibration::SessionId session_id,
                                                              QTreeWidget *filesTreeWidget,
                                                              const fastecu::calibration::CalibrationSession& session)
{
    QTreeWidget *calFilesTree = filesTreeWidget;
    calFilesTree->setAnimated(true);
    calFilesTree->setFocusPolicy(Qt::NoFocus);
    QFont filesItemFont;
    filesItemFont.setPixelSize(14);
    calFilesTree->setFont(filesItemFont);

    QTreeWidgetItem *topLevelFilesTreeItem = new QTreeWidgetItem();
    topLevelFilesTreeItem->setText(2, fastecu::ui::session_key_text(session_id));
    topLevelFilesTreeItem->setCheckState(0, Qt::Unchecked);
    topLevelFilesTreeItem->setFirstColumnSpanned(true);
    calFilesTree->addTopLevelItem(topLevelFilesTreeItem);

    topLevelFilesTreeItem->setText(0, QString::fromStdString(session.source().display_name));
    if (const auto *resolved = session.definition(); resolved != nullptr && !resolved->definition.maps.empty())
    {
        topLevelFilesTreeItem->setText(1, legacy_value(resolved->definition.maps.front().id));
    }
    calFilesTree->expandItem(topLevelFilesTreeItem);

    for (int i = 0; i < calFilesTree->topLevelItemCount(); i++)
    {
        calFilesTree->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
        calFilesTree->topLevelItem(i)->setSelected(false);
    }
    topLevelFilesTreeItem->setSelected(true);
    topLevelFilesTreeItem->setCheckState(0, Qt::Checked);

    return calFilesTree;
}

QTreeWidget *CalibrationTreeWidget::buildCalibrationDataTree(QTreeWidget *dataTreeWidget,
                                                             const fastecu::calibration::CalibrationSession& session,
                                                             const fastecu::ui::CalibrationViewState& view)
{
    dataTreeWidget->clear();

    QTreeWidget *calDataTree = dataTreeWidget;
    calDataTree->setAnimated(true);
    calDataTree->setFocusPolicy(Qt::NoFocus);
    QFont dataItemFont;
    dataItemFont.setPixelSize(12);
    calDataTree->setFont(dataItemFont);

    const QStringList labels = fastecu::ui::rom_info_labels();
    const QStringList values = fastecu::ui::rom_info_values(session, view.missing_definition_make);
    QTreeWidgetItem *romInfoItem = new QTreeWidgetItem();
    romInfoItem->setText(0, "ROM Info");
    calDataTree->addTopLevelItem(romInfoItem);
    if (view.rom_info_expanded)
    {
        romInfoItem->setExpanded(true);
    }
    for (int i = 0; i < values.length(); i++)
    {
        emit LOG_D("Set " + labels.at(i) + ": " + values.at(i), true, true);
        QTreeWidgetItem *item = new QTreeWidgetItem();
        item->setText(0, labels.at(i) + ": " + values.at(i));
        calDataTree->topLevelItem(0)->addChild(item);
    }

    const fastecu::calibration::ResolvedDefinition *resolved = session.definition();
    const std::size_t map_count = resolved != nullptr ? resolved->definition.maps.size() : 0;
    for (std::size_t j = 0; j < map_count; j++)
    {
        const fastecu::definition::CalibrationMap& map = resolved->definition.maps[j];
        const QString category = legacy_value(map.category);
        const QString name = legacy_value(map.name);
        if (category == " " || name == " ")
        {
            continue;
        }
        bool treeCategoryCreated = false;
        for (int i = 0; i < calDataTree->topLevelItemCount(); i++)
        {
            if (calDataTree->topLevelItem(i)->text(0) == category)
            {
                treeCategoryCreated = true;
            }
        }
        if (!treeCategoryCreated)
        {
            QTreeWidgetItem *categoryItem = new QTreeWidgetItem();
            categoryItem->setText(0, category);
            calDataTree->addTopLevelItem(categoryItem);
            if (view.expanded_categories.contains(category))
            {
                categoryItem->setExpanded(true);
            }
        }
        const QString type = legacy_value(map.type);
        for (int i = 0; i < calDataTree->topLevelItemCount(); i++)
        {
            if (calDataTree->topLevelItem(i)->text(0) != category)
            {
                continue;
            }
            QTreeWidgetItem *item = new QTreeWidgetItem();
            if (type == "1D" || type == "Selectable" || (map.y_size == 1 && map.x_size == 1))
            {
                item->setIcon(0, QIcon(":/icons/1D-64.png"));
            }
            else if (type == "2D")
            {
                item->setIcon(0, QIcon(":/icons/2D-64.png"));
            }
            else if (type == "3D")
            {
                item->setIcon(0, QIcon(":/icons/3D-64.png"));
            }
            item->setCheckState(0, view.open_maps.contains(j) ? Qt::Checked : Qt::Unchecked);
            item->setText(0, name);
            item->setText(1, QString::number(j));
            calDataTree->topLevelItem(i)->addChild(item);
            item->setToolTip(0, name + legacy_value(map.description));
        }
    }

    return calDataTree;
}
```

Add `#include "src/ui/desktop/calibration/rom_info.h"`. Add `"//src/ui/desktop/calibration:calibration_view_state"` and `"//src/ui/desktop/calibration:rom_info"` to the `desktop` target deps.

`mainwindow.cpp` will not compile until Task 5 rewires the callers, so this task runs only the tree test, which builds `:desktop`. **Do Task 5's Step 4 edits for the four tree call sites in `mainwindow.cpp` as part of this task, if needed, to get `:desktop` building.** Record that as a ruling.

- [ ] **Step 4: Run to verify.** Run `bazel test --config=release //src/ui/desktop:test_calibration_treewidget`. Expected: PASS.

- [ ] **Step 5: Mutation check.** Drop the `(map.y_size == 1 && map.x_size == 1)` clause temporarily. `dataTreeMatchesLegacyRules` must fail on `Tiny`'s icon. Revert.

- [ ] **Step 6: Commit** (together with Task 5 if the build forced the merge; otherwise separately)

```bash
git add src/ui/desktop
git commit -m "feat(ui): build the calibration trees from the session (6m-3)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 5: `MainWindow` keeps view state and writes metadata through the session

**Files:**
- Modify: `src/ui/desktop/mainwindow.{h,cpp}`, `src/ui/desktop/mainwindow_test.cpp`

**Interfaces:**
- `MainWindow::OpenCalibration` gains `fastecu::ui::CalibrationViewState view;`.
- New private helpers:
  - `OpenCalibration *open_calibration(fastecu::calibration::SessionId id);`
  - `OpenCalibration *selected_open_calibration();`, whose `selected_legacy_calibration()` now delegates to it.
- `void prompt_for_missing_definition(fastecu::calibration::SessionId id);`, which was `(EcuCalDefStructure*)`.
- A private helper, `void set_category_expanded(QTreeWidgetItem *item, bool expanded);`.

- [ ] **Step 1: Write the failing tests** (`mainwindow_test.cpp`)

Change the placeholder assertions in `definitionlessOpenPromptsOnceAndAppliesPlaceholders` to read the tree instead of the legacy struct:

```cpp
        QTreeWidgetItem *rom_info = window.ui->calibrationDataTreeWidget->topLevelItem(0);
        QCOMPARE(rom_info->text(0), QString("ROM Info"));
        QCOMPARE(rom_info->child(0)->text(0), QString("XML ID: UnknownID"));
        QCOMPARE(rom_info->child(4)->text(0),
                 "Make: " + QString::fromStdString(services.config.selected_vehicle()->make));
        QCOMPARE(window.calibrations_.front().view.missing_definition_make,
                 std::optional<QString>(QString::fromStdString(services.config.selected_vehicle()->make)));
```

Remove its `legacy.RomInfo.at(FileActions::XmlId)` check.

Add:

```cpp
    void closingARomClosesAllOfItsWindows()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QTemporaryDir roms;
        ModalDriver driver{QString()};
        driver.start();
        QCOMPARE(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
        QCOMPARE(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
        driver.stop();
        const QString a_key = fastecu::ui::session_key_text(window.calibrations_.at(0).id);
        const QString b_key = fastecu::ui::session_key_text(window.calibrations_.at(1).id);
        for (const QString& name : {a_key + ",0,X", a_key + ",1,Y", b_key + ",0,Z"})
        {
            auto *content = new QWidget;
            QMdiSubWindow *sub = window.ui->mdiArea->addSubWindow(content);
            sub->setObjectName(name);
        }
        // The data tree still shows b; select a's row directly, as keyboard
        // navigation would, without rebuilding the data tree.
        QTreeWidget *files = window.ui->calibrationFilesTreeWidget;
        files->topLevelItem(0)->setSelected(true);
        files->topLevelItem(1)->setSelected(false);

        window.close_calibration();

        QStringList remaining;
        for (QMdiSubWindow *sub : window.ui->mdiArea->subWindowList())
        {
            remaining << sub->objectName();
        }
        QCOMPARE(remaining, QStringList{b_key + ",0,Z"});
    }

    void viewStateIsKeptPerRom()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QTemporaryDir roms;
        ModalDriver driver{QString()};
        driver.start();
        QCOMPARE(window.open_calibration_file(writeRom(roms, "a.bin", '\x0a')), 0);
        QCOMPARE(window.open_calibration_file(writeRom(roms, "b.bin", '\x0b')), 0);
        driver.stop();
        QTreeWidget *files = window.ui->calibrationFilesTreeWidget;
        QTreeWidget *data = window.ui->calibrationDataTreeWidget;
        const auto select_rom = [&](int row)
        {
            for (int i = 0; i < files->topLevelItemCount(); ++i)
            {
                files->topLevelItem(i)->setSelected(i == row);
            }
            window.calibration_files_treewidget_item_selected(files->topLevelItem(row));
        };

        select_rom(0);
        window.calibration_data_treewidget_item_expanded(data->topLevelItem(0)); // ROM Info
        select_rom(1);
        QVERIFY(!data->topLevelItem(0)->isExpanded());
        select_rom(0);
        QVERIFY(data->topLevelItem(0)->isExpanded());
        QVERIFY(window.calibrations_.at(0).view.rom_info_expanded);
        QVERIFY(!window.calibrations_.at(1).view.rom_info_expanded);
    }
```

- [ ] **Step 2: Run to verify they fail.** Run `bazel test --config=release //src/ui/desktop:test_mainwindow`. Expected: FAIL to compile (`view` member), or, once the member exists, the new slots fail.

- [ ] **Step 3: Header**
  - `#include "src/ui/desktop/calibration/calibration_view_state.h"`.
  - Add `fastecu::ui::CalibrationViewState view;` to `OpenCalibration`.
  - Declare the helpers from the Interfaces block.
  - Change `prompt_for_missing_definition`'s parameter.

- [ ] **Step 4: `mainwindow.cpp`**

Helpers:

```cpp
MainWindow::OpenCalibration *MainWindow::open_calibration(fastecu::calibration::SessionId id)
{
    const auto found =
        std::ranges::find_if(calibrations_, [id](const OpenCalibration& open) { return open.id == id; });
    return found == calibrations_.end() ? nullptr : &*found;
}

MainWindow::OpenCalibration *MainWindow::selected_open_calibration()
{
    const QList<QTreeWidgetItem *> selected = ui->calibrationFilesTreeWidget->selectedItems();
    const auto id = selected.isEmpty() ? std::nullopt : session_of(selected.at(0));
    return id.has_value() ? open_calibration(*id) : nullptr;
}
```

`legacy_calibration(id)` returns `open ? open->legacy.get() : nullptr` via `open_calibration`. `selected_legacy_calibration()` returns `selected_open_calibration()`'s legacy or `nullptr`.

`add_calibration`: after `calibrations_.push_back(...)`, replace the rest with:

```cpp
    update_protocol_info(fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(*session),
                                                     fastecu::ui::RomInfoRow::FlashMethod));
    if (session->definition() == nullptr)
    {
        prompt_for_missing_definition(id);
    }
    const OpenCalibration *open = open_calibration(id);
    calibrationTreeWidget->buildCalibrationFilesTree(id, ui->calibrationFilesTreeWidget, *session);
    calibrationTreeWidget->buildCalibrationDataTree(ui->calibrationDataTreeWidget, *session, open->view);
    return true;
```

`legacy->use_romraider_definition || use_ecuflash_definition` holds exactly when the session has a definition, which is why the condition becomes `definition() == nullptr`.

`prompt_for_missing_definition(fastecu::calibration::SessionId id)`: the dialog code is unchanged. The two authoring calls take no argument (Task 3). The continue-without branch becomes:

```cpp
    if (continueWithoutRadioButton->isChecked() || result == QDialog::Rejected)
    {
        emit LOG_D(continueWithoutRadioButton->text(), true, true);
        if (OpenCalibration *open = open_calibration(id); open != nullptr)
        {
            open->view.missing_definition_make = qs(selected_vehicle().make);
        }
    }
```

`calibration_files_treewidget_item_selected`:

```cpp
    OpenCalibration *open = selected_open_calibration();
    const fastecu::calibration::CalibrationSession *session =
        open != nullptr ? calibrationWorkspace->find(open->id) : nullptr;
    if (session == nullptr)
    {
        return;
    }
    calibrationTreeWidget->buildCalibrationDataTree(ui->calibrationDataTreeWidget, *session, open->view);
    update_protocol_info(fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(*session),
                                                     fastecu::ui::RomInfoRow::FlashMethod));
```

This replaces the `legacy` lookup and its two uses.

`calibration_data_treewidget_item_selected`:
- Resolve `OpenCalibration *open = open_calibration(*session);` next to `legacy`, and return if it is null.
- Replace `legacy->VisibleList.at(i) == "1"` with `open->view.open_maps.contains(static_cast<std::size_t>(i))`.
- Replace `legacy->VisibleList.replace(i, "0")` with `open->view.open_maps.erase(static_cast<std::size_t>(i))`.
- Replace `legacy->VisibleList.replace(i, "1")` with `open->view.open_maps.insert(static_cast<std::size_t>(i))`.

The expanded and collapsed handlers become:

```cpp
void MainWindow::calibration_data_treewidget_item_expanded(QTreeWidgetItem *item)
{
    set_category_expanded(item, true);
}

void MainWindow::calibration_data_treewidget_item_collapsed(QTreeWidgetItem *item)
{
    set_category_expanded(item, false);
}

void MainWindow::set_category_expanded(QTreeWidgetItem *item, bool expanded)
{
    const int itemIndex = ui->calibrationDataTreeWidget->indexOfTopLevelItem(item);
    OpenCalibration *open = selected_open_calibration();
    if (itemIndex < 0 || open == nullptr)
    {
        return;
    }
    const QString categoryName = ui->calibrationDataTreeWidget->topLevelItem(itemIndex)->text(0);
    if (expanded)
    {
        open->view.expanded_categories.insert(categoryName);
    }
    else
    {
        open->view.expanded_categories.erase(categoryName);
    }
    if (categoryName == "ROM Info")
    {
        open->view.rom_info_expanded = expanded;
    }
}
```

`close_calibration`: replace the triple window loop with a close-by-key sweep:

```cpp
    const QString windowPrefix = romKey + ",";
    for (QMdiSubWindow *w : ui->mdiArea->subWindowList())
    {
        if (w->objectName().startsWith(windowPrefix))
        {
            ui->mdiArea->removeSubWindow(w);
        }
    }
```

`close_calibration_map`:
- Resolve `OpenCalibration *open = id.has_value() ? open_calibration(*id) : nullptr;` in place of `legacy`, and keep the early return.
- The loop keeps matching `open->legacy->NameList` (map columns stay legacy until 6m-4), but erases from `open->view.open_maps` instead of writing `VisibleList`.

In the write branch of `start_ecu_operations`, replace the protocol-field block (from `if (legacy->RomInfo.at(fileActions->FlashMethod) == "")` through `legacy->McuType = ...;`) with:

```cpp
            OpenCalibration *open = selected_open_calibration();
            fastecu::calibration::CalibrationSession *session = calibrationWorkspace->find(open->id);
            fastecu::calibration::RomProtocolInfo protocol = session->protocol();
            // Legacy filled an empty flash method -- "" only when a definition
            // left it empty; a definition-less ROM shows " " -- then refreshed
            // the vehicle selection before taking kernel/MCU from it.
            if (fastecu::ui::rom_info_value(fastecu::ui::rom_info_values(*session),
                                            fastecu::ui::RomInfoRow::FlashMethod)
                    .isEmpty())
            {
                protocol.flash_method = selected_vehicle().protocol_name;
                session->set_protocol(protocol);
                update_protocol_info(qs(protocol.flash_method));
            }
            protocol.kernel_path = fastecu::flash::kernel_path(
                kernel_dir.toStdString(),
                fastecu::config::protocol_field_or_placeholder(selected_vehicle(), &ProtocolEntry::kernel));
            protocol.kernel_start_address = protocol_field(selected_vehicle(), &ProtocolEntry::kernel_addr).toStdString();
            protocol.mcu_type = protocol_field(selected_vehicle(), &ProtocolEntry::mcu).toStdString();
            session->set_protocol(protocol);
            fastecu::ui::refresh_legacy_metadata(*session, *legacy);
            legacy->FlashMethod = qs(selected_vehicle().protocol_name);
```

`open` is non-null here because `legacy` was just resolved from the same selection. `protocol_field` (`config_fields.h`) returns `QString`, hence the `.toStdString()`.

Remove `fileActions->apply_missing_definition_defaults` (no caller left). The `FileActions` method itself stays until 6m-5. Remove every call to the deleted `calibrationDataTreeWidgetItemExpanded/Collapsed`.

- [ ] **Step 5: Run to verify they pass.** Run `bazel test --config=release //src/ui/desktop:all //src/ui/desktop/calibration:all //apps/desktop:all`. Expected: PASS. Read `bazel-testlogs/src/ui/desktop/test_mainwindow/test.log` and confirm each new slot passes without timeout.

- [ ] **Step 6: Mutation check.** Restore the old triple loop's behavior temporarily: close only windows whose map name appears in the current data tree. `closingARomClosesAllOfItsWindows` must fail. Revert.

- [ ] **Step 7: Commit**

```bash
git add src/ui/desktop
git commit -m "feat(ui): keep calibration view state per session and write ROM metadata through it (6m-3)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 6: Documentation, gates, publish

- [ ] **Step 1:** In `docs/modularization-plan.md`'s 6m progress paragraph, replace "Rendering, every mutation, and retirement of the legacy model remain for 6m-3" with:

  > 6m-3 renders the calibration trees and ROM info from the session, keeps per-ROM view state in the UI, and makes the session's protocol info the metadata truth. Map windows, edits, save, write and checksum move to session bytes in 6m-4; 6m-5 retires the legacy model.

- [ ] **Step 2: Gates**

```bash
bazel build -k --config=release //...
bazel test -k --config=release //...
prek run --all-files
bazel run --config=release //:clang_tidy_report_changed
```

  Fix findings in touched files; convert rather than `NOLINT`.

- [ ] **Step 3: Commit**

```bash
git add docs/modularization-plan.md
git commit -m "docs: record step 6m-3 progress

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 4: Publish after the user authorizes the push:** `gh stack init --base master feat/6m-3-calibration-tree`, then `gh stack submit --auto --open`. Title `6m-3: Render calibration trees and ROM info from the session`, with a What/Why/Validation/References body ending with the Claude Code attribution line.
