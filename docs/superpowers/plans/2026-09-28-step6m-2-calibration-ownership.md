# Step 6m-2: Calibration Ownership and Identity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move ownership of open calibrations from `MainWindow`'s 100-slot raw-pointer array to the `CalibrationWorkspace`. Address them by `SessionId` everywhere instead of by tree position. Keep every existing reader and writer working unchanged on one legacy view per session.

**Architecture:**
- `MainWindow` receives the workspace through `MainWindowServices`. It keeps, in files-tree order, one `OpenCalibration { SessionId; std::unique_ptr<EcuCalDefStructure> legacy; }` per session.
- The legacy view is built once at open by a UI-side projection, `project_legacy_calibration`. That projection is golden-equivalence tested against what `FileActions::open_subaru_rom_file` produced.
- Tree rows and map windows carry the `SessionId` as a decimal key, so closing a ROM renumbers nothing.
- The post-read handoff adopts the image into the workspace only after a successful read.
- `HexEdit` takes bytes by value.
- Map rendering, edits, save, write and checksum are unchanged; they read and write the legacy view, which 6m-3 retires.

**Tech Stack:** C++23, Bazel (version in `.bazelversion`), Qt 6 widgets, QtTest (`fastecu_qttest`), GoogleTest (`fastecu_gtest`, `fastecu_portable_gtest`), prek, `gh stack`.

**Spec:** [Step 6m — Definition and calibration sessions](../specs/2026-09-28-step6m-calibration-sessions-design.md), section "6m-2 — Ownership and identity".

## Global Constraints

- **The legacy view is the only byte store the UI reads or writes in this slice.** Nothing may read `CalibrationSession::rom()` after the projection has run, and nothing calls `write_bytes`. 6m-3 reverses this.
- **Behavior is preserved.** The known improvements are:
  - IDs replace positions, so no renumbering.
  - A closed ROM's legacy view is freed; legacy leaked it.
  - Save with nothing selected shows the existing "No calibration to save!" message instead of indexing an empty selection.
- Map window object names keep their legacy shape `"<key>,<map index>,<map name>"`. `<key>` changes from a position to `session_key_text(id)`, the decimal `SessionId`. The files tree keeps the key in column 2, where the position used to be.
- Operator-visible text (messages, notices, `LOG_*` lines) is unchanged.
- Backend results are checked with `.has_value()`. No new `ErrorKind`. No `qt_layer` entry is added, and the ratchet only shrinks.
- Platform differences go in separate sources, and guards are spelled `_WIN32`. Markdown cross-references are links.
- Tests use synthetic ROM bytes only.
- No wire behavior, flash sequence, definition schema or ROM file format changes. No bench checklist changes.
- Branch: `feat/6m-2-calibration-ownership`, one PR onto `master` via `gh stack init --base master feat/6m-2-calibration-ownership` and `gh stack submit --auto --open`, after the user authorizes the push.
- Every PR passes `bazel build -k --config=release //...`, `bazel test -k --config=release //...`, `prek run --all-files`, and `bazel run --config=release //:clang_tidy_report_changed`. Windows/macOS/Linux CI is required, and anything not run locally is reported as pending.
- Every commit message ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

## Review Focus

- **Closing a ROM that is not the last one while a later ROM's map window stays open.** Expected: the later window still resolves to its own ROM, no row is renumbered, and edits land in the right image. Pinned by `MainWindowTest::closingAMiddleRomKeepsLaterRomsAddressable` (Task 6).
- **A map window or its `destroyed` signal outliving its ROM.** Expected: the handlers find no session and do nothing, with no crash and no selection change. Pinned by `MainWindowTest::windowsOfAClosedRomAreInert` (Task 6).
- **A failed or cancelled ECU read.** Expected: no tree row, no session, no legacy view. Pinned by `MainWindowTest::readOfAnUnsupportedProtocolAddsNoCalibration` and the Denso TCU cleanup test (Task 6).
- **Opening a ROM with no matching definition.** Expected: the missing-definition prompt appears exactly once, and dismissing it applies the legacy placeholders (`UnknownID`). Pinned by `MainWindowTest::definitionlessOpenPromptsOnceAndAppliesPlaceholders` (Task 6).
- **A definition that addresses past the end of the image.** Expected: the ROM opens with its ROM info and no maps, exactly as legacy did. Pinned by `LegacyCalibrationView.SizeRejectedDefinitionShowsNoMaps` (Task 4).

---

## File Structure

| File | Change | Task |
|---|---|---|
| Spec (6m-2/6m-3 re-slice, checksum correction) and this plan | Commit | 0 |
| `src/backend/definition/legacy/legacy_definition_adapter.{h,cpp}`, `legacy_definition_adapter_test.cpp` | Extract `project_definition` | 1 |
| `src/ui/desktop/calibration/session_key.{h,cpp}`, `session_key_test.cpp`, `BUILD.bazel` | Create | 2 |
| `src/ui/desktop/calibration/map_edit_adapter.{h,cpp}`, `map_edit_adapter_test.cpp` | `MapWindowId` by `SessionId` | 2 |
| `src/ui/desktop/hexedit/hexedit.{h,cpp}` | Take bytes and name by value | 3 |
| `src/ui/desktop/calibration/legacy_calibration_view.{h,cpp}`, `legacy_calibration_view_test.cpp` | Create (projection) | 4 |
| `src/ui/desktop/main_window_services.h`, `apps/desktop/desktop_composition.cpp`, `apps/desktop/desktop_composition_test.cpp` | Services carry the workspace | 5 |
| `src/ui/desktop/mainwindow.{h,cpp}`, `menu_actions.cpp`, `calibration_maps.{h,cpp}`, `calibration_treewidget.{h,cpp}`, `BUILD.bazel`, `mainwindow_test.cpp` | Cut over to sessions | 6 |
| `docs/modularization-plan.md` | Record 6m-1/6m-2 progress | 7 |

---

### Task 0: Commit the spec re-slice and this plan

**Files:** the spec and this plan, both already edited on `feat/6m-2-calibration-ownership`.

- [ ] **Step 1: Commit**

```bash
git add docs/superpowers
git commit -m "docs: re-slice step 6m and plan 6m-2 calibration ownership

The per-call projection 6m-2 assumed would have been a second long-lived
byte store: map windows re-render from the text edits patch, and the
switch/selectable handlers write bytes directly. 6m-2 now moves ownership
and identity only; rendering and every mutation move together in 6m-3.
Also corrects the spec: legacy restores the pre-checksum image after a
completed write, it does not keep the corrected checksums.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 1: `LegacyDefinitionAdapter::project_definition`

**Files:**
- Modify: `src/backend/definition/legacy/legacy_definition_adapter.h`, `legacy_definition_adapter.cpp:391-432`
- Test: `src/backend/definition/legacy/legacy_definition_adapter_test.cpp`

**Interfaces:**
- Produces: `static Status LegacyDefinitionAdapter::project_definition(definitions::EcuCalDefStructure& current, const RomDefinition& definition, DefinitionFormat format);`
  - It is the part of `replace_definition` after the load: clear the map and scaling rows, populate `RomInfo`, set `use_*_definition`, append the scalings and maps, and validate alignment.
  - It is atomic: on failure `current` is unchanged.
  - `replace_definition` calls it, so its own behavior is unchanged.

- [ ] **Step 1: Write the failing test**

Append to `legacy_definition_adapter_test.cpp`, inside its anonymous namespace:

```cpp
TEST_F(LegacyDefinitionAdapterTest, ProjectingALoadedDefinitionEqualsReplacingIt)
{
    repository.files["one.xml"] = bytes(R"xml(
      <rom><romid><xmlid>ONE</xmlid><internalidaddress>100</internalidaddress>
        <internalidstring>ONE-ID</internalidstring><make>Subaru</make>
        <flashmethod>denso</flashmethod></romid>
        <scaling name="raw" toexpr="x" frexpr="x" format="%d" storagetype="uint8" endian="big"/>
        <table name="Idle" address="20" type="1D" category="Idle" scaling="raw" storagetype="uint8"/>
      </rom>)xml");
    auto catalog = DefinitionCatalog::create({DefinitionIndexEntry{
        .format = DefinitionFormat::EcuFlash,
        .definition_id = "ONE",
        .internal_id = "ONE-ID",
        .internal_id_address = 0x100,
        .internal_id_encoding = IdEncoding::Ascii,
        .source = "one.xml",
    }});
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    definitions::EcuCalDefStructure replaced;
    ASSERT_THAT(adapter.replace_definition(replaced, *catalog, DefinitionFormat::EcuFlash, "ONE"),
                fastecu::testing::IsOk());
    auto loaded = service.load(*catalog, DefinitionFormat::EcuFlash, "ONE");
    ASSERT_THAT(loaded, fastecu::testing::IsOk());

    definitions::EcuCalDefStructure projected;
    ASSERT_THAT(LegacyDefinitionAdapter::project_definition(projected, *loaded, DefinitionFormat::EcuFlash),
                fastecu::testing::IsOk());

    EXPECT_EQ(projected, replaced);
    EXPECT_TRUE(projected.use_ecuflash_definition);
    EXPECT_EQ(projected.NameList, QStringList{"Idle"});
}

TEST_F(LegacyDefinitionAdapterTest, AFailedProjectionLeavesTheValueUnchanged)
{
    definitions::EcuCalDefStructure value;
    value.RomInfoStrings = {"too", "short"};
    value.FileName = "kept.bin";
    const definitions::EcuCalDefStructure before = value;

    EXPECT_THAT(LegacyDefinitionAdapter::project_definition(value, RomDefinition{}, DefinitionFormat::EcuFlash),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(value, before);
}
```

Add `using` lines for `IdEncoding`/`ErrorKind` if the file lacks them. It already uses `DefinitionCatalog` and `DefinitionIndexEntry` unqualified inside `namespace fastecu::definition`.

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test --config=release //src/backend/definition/legacy:test_legacy_definition_adapter`
Expected: FAIL to compile with `no member named 'project_definition'`.

- [ ] **Step 3: Extract the function**

In `legacy_definition_adapter.h`, add to the public section of `LegacyDefinitionAdapter`:

```cpp
    // Replaces `current`'s definition columns with `definition`'s -- the part
    // of replace_definition after the load. Atomic: on failure `current` is
    // unchanged. Used to build the transitional legacy view of an already
    // resolved calibration session (step 6m-2).
    static Status project_definition(definitions::EcuCalDefStructure& current, const RomDefinition& definition,
                                     DefinitionFormat format);
```

In `legacy_definition_adapter.cpp`, replace the body of `replace_definition` between the load and the `resolved` hand-back with a call, and define the new function from the moved code:

```cpp
Status LegacyDefinitionAdapter::project_definition(definitions::EcuCalDefStructure& current,
                                                   const RomDefinition& definition, DefinitionFormat format)
{
    definitions::EcuCalDefStructure next = current;
    clear_map_rows(next);
    clear_scaling_rows(next);
    auto rom_info = populate_rom_info(next, definition);
    if (!rom_info.has_value())
    {
        return rom_info;
    }
    next.use_romraider_definition = format == DefinitionFormat::RomRaider;
    next.use_ecuflash_definition = format == DefinitionFormat::EcuFlash;
    for (const Scaling& scaling : definition.scalings)
    {
        append_scaling(next, scaling);
    }
    for (const CalibrationMap& map : definition.maps)
    {
        append_map(next, definition, map);
    }
    auto aligned = validate_definition_alignment(next);
    if (!aligned.has_value())
    {
        return aligned;
    }
    current = std::move(next);
    return {};
}

Status LegacyDefinitionAdapter::replace_definition(definitions::EcuCalDefStructure& current,
                                                   const DefinitionCatalog& catalog, DefinitionFormat format,
                                                   std::string_view id, RomDefinition *resolved)
{
    auto definition = service_.load(catalog, format, id);
    if (!definition.has_value())
    {
        return std::unexpected(definition.error());
    }
    if (auto projected = project_definition(current, *definition, format); !projected.has_value())
    {
        return projected;
    }
    // Hand the already-resolved definition back when the caller asked for it,
    // so nothing downstream has to rebuild a catalog and load it a second time.
    if (resolved != nullptr)
    {
        *resolved = std::move(*definition);
    }
    return {};
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `bazel test --config=release //src/backend/definition/legacy:all //src/backend/definitions:all`
Expected: PASS, including every pre-existing adapter and `FileActions` test.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definition/legacy
git commit -m "refactor(definition): split legacy definition projection from loading (6m-2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 2: Session keys and map-window IDs

**Files:**
- Create: `src/ui/desktop/calibration/session_key.h`, `session_key.cpp`, `session_key_test.cpp`
- Modify: `src/ui/desktop/calibration/BUILD.bazel`, `map_edit_adapter.h:113-121`, `map_edit_adapter.cpp:212-225`, `map_edit_adapter_test.cpp:292-318`

**Interfaces:**
- Produces, in namespace `fastecu::ui`:
  - `QString session_key_text(calibration::SessionId id)`: decimal text of the ID.
  - `std::optional<calibration::SessionId> parse_session_key(const QString& text)`: strict decimal, `nullopt` for anything else, including an empty string, a sign, or trailing text.
- Changes: `MapWindowId` becomes `{ calibration::SessionId session; int map_number; }`. `parse_map_window_id` returns `nullopt` when field 0 is not a session key.

- [ ] **Step 1: Write the failing tests**

Create `src/ui/desktop/calibration/session_key_test.cpp`:

```cpp
#include "src/ui/desktop/calibration/session_key.h"

#include <gtest/gtest.h>

namespace fastecu::ui
{
namespace
{

using calibration::SessionId;

TEST(SessionKey, RoundTripsDecimalText)
{
    EXPECT_EQ(session_key_text(SessionId{42}), QString("42"));
    EXPECT_EQ(parse_session_key("42"), SessionId{42});
    EXPECT_EQ(parse_session_key(session_key_text(SessionId{18446744073709551615ULL})),
              SessionId{18446744073709551615ULL});
}

TEST(SessionKey, RejectsAnythingButPlainDecimal)
{
    EXPECT_FALSE(parse_session_key("").has_value());
    EXPECT_FALSE(parse_session_key("-1").has_value());
    EXPECT_FALSE(parse_session_key("+1").has_value());
    EXPECT_FALSE(parse_session_key("1a").has_value());
    EXPECT_FALSE(parse_session_key(" 1").has_value());
    EXPECT_FALSE(parse_session_key("18446744073709551616").has_value()); // overflow
}

} // namespace
} // namespace fastecu::ui
```

In `map_edit_adapter_test.cpp`, replace `ParsesRomAndMapNumberFromAWellFormedObjectName` and add a rejection test:

```cpp
TEST(ParseMapWindowId, ParsesSessionAndMapNumberFromAWellFormedObjectName)
{
    QMdiSubWindow window;
    window.setObjectName("12,7,Timing,uint16");

    const auto id = parse_map_window_id(&window);

    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->session, calibration::SessionId{12});
    EXPECT_EQ(id->map_number, 7);
}

TEST(ParseMapWindowId, ReturnsNulloptWhenTheSessionKeyIsNotDecimal)
{
    QMdiSubWindow window;
    window.setObjectName("x,7,Timing");

    EXPECT_FALSE(parse_map_window_id(&window).has_value());
}
```

In `src/ui/desktop/calibration/BUILD.bazel`, add:

```python
qt_cc_library(
    name = "session_key",
    srcs = ["session_key.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["session_key.h"],
    deps = QT_DEPS + ["//src/backend/calibration/session:calibration_session"],
)

fastecu_gtest(
    name = "session_key_test",
    srcs = ["session_key_test.cpp"],
    deps = [":session_key"],
)
```

Then add `":session_key"` to `map_edit_adapter`'s `deps`.

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/ui/desktop/calibration:all`
Expected: FAIL to build, because `session_key.h` does not exist and `MapWindowId` has no member `session`.

- [ ] **Step 3: Implement**

Create `src/ui/desktop/calibration/session_key.h`:

```cpp
#pragma once

#include <optional>

#include <QString>

#include "src/backend/calibration/session/calibration_session.h"

namespace fastecu::ui
{

// How a calibration session is named in the widgets that refer to it: the
// files tree's column 2 and the first field of a map window's object name.
// Decimal, so the object-name shape "<key>,<map>,<name>" is unchanged from the
// positional index it replaces.
QString session_key_text(calibration::SessionId id);

// Strict inverse of session_key_text: plain decimal digits only.
std::optional<calibration::SessionId> parse_session_key(const QString& text);

} // namespace fastecu::ui
```

Create `src/ui/desktop/calibration/session_key.cpp`:

```cpp
#include "src/ui/desktop/calibration/session_key.h"

#include <algorithm>
#include <cstdint>

namespace fastecu::ui
{

QString session_key_text(calibration::SessionId id)
{
    return QString::number(static_cast<qulonglong>(static_cast<std::uint64_t>(id)));
}

std::optional<calibration::SessionId> parse_session_key(const QString& text)
{
    if (text.isEmpty() || !std::ranges::all_of(text, [](QChar c) { return c >= u'0' && c <= u'9'; }))
    {
        return std::nullopt;
    }
    bool ok = false;
    const qulonglong value = text.toULongLong(&ok);
    if (!ok)
    {
        return std::nullopt;
    }
    return calibration::SessionId{static_cast<std::uint64_t>(value)};
}

} // namespace fastecu::ui
```

In `map_edit_adapter.h`, add `#include "src/ui/desktop/calibration/session_key.h"` and change the struct:

```cpp
struct MapWindowId
{
    calibration::SessionId session{};
    int map_number{0};
};

// Returns nullopt for a null window, an object name with fewer than the two
// leading comma-separated fields this needs, or a first field that is not a
// session key.
std::optional<MapWindowId> parse_map_window_id(QMdiSubWindow *window);
```

In `map_edit_adapter.cpp`, replace the final `return` of `parse_map_window_id`:

```cpp
    const std::optional<calibration::SessionId> session = parse_session_key(parts.at(0));
    if (!session.has_value())
    {
        return std::nullopt;
    }
    return MapWindowId{.session = *session, .map_number = parts.at(1).toInt()};
```

The remaining `build_map_window` helper in the test names windows `"0,0,Timing,uint16"`. `"0"` still parses as a key, so leave it.

- [ ] **Step 4: Run to verify they pass**

Run: `bazel test --config=release //src/ui/desktop/calibration:all`
Expected: PASS. `//src/ui/desktop:desktop` does not build yet, because `menu_actions.cpp` still reads `id->rom_number`. Task 6 fixes that; do not build `//...` in this task.

- [ ] **Step 5: Commit**

```bash
git add src/ui/desktop/calibration
git commit -m "feat(ui): name calibration sessions by stable key in map windows (6m-2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3: `HexEdit` takes the image by value

**Files:**
- Modify: `src/ui/desktop/hexedit/hexedit.h:37,64`, `src/ui/desktop/hexedit/hexedit.cpp:6-17`, and the hexedit `BUILD.bazel` if it depends on `//src/backend/definitions` only for this

**Interfaces:**
- Produces: `HexEdit(const QByteArray& data, const QString& file_name, QWidget *parent = nullptr)`. The `ecuCalDef` member is removed.

`HexEdit` only read the struct in its constructor. Keeping the pointer is what would dangle once a ROM's legacy view is freed on close. No behavior test is possible for a removed member, so the compiler plus Task 6's `hexEditorOutlivesItsRom` test are the checks.

- [ ] **Step 1: Change the constructor**

`hexedit.h`: replace the constructor declaration with

```cpp
    HexEdit(const QByteArray& data, const QString& file_name, QWidget *parent = nullptr);
```

Then delete the `FileActions::EcuCalDefStructure *ecuCalDef;` member and the `file_actions.h` include if nothing else uses it.

`hexedit.cpp`:

```cpp
HexEdit::HexEdit(const QByteArray& data, const QString& file_name, QWidget *parent) : QMainWindow(parent)
{
    setAcceptDrops(true);
    init();
    setCurrentFile("");

    hexEdit->setData(data);
    setCurrentFile(file_name);

    this->show();
}
```

Run `grep -n "ecuCalDef" src/ui/desktop/hexedit/*` and confirm no match remains. If the hexedit `BUILD.bazel` lists `//src/backend/definitions` and nothing else in the package includes it, drop that dep.

- [ ] **Step 2: Build the package**

Run: `bazel build --config=release //src/ui/desktop/hexedit:all`
Expected: builds. The only caller, `menu_actions.cpp`, is updated in Task 6.

- [ ] **Step 3: Commit**

```bash
git add src/ui/desktop/hexedit
git commit -m "refactor(ui): hand the hex editor its image by value (6m-2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 4: The legacy view projection

**Files:**
- Create: `src/ui/desktop/calibration/legacy_calibration_view.h`, `legacy_calibration_view.cpp`, `legacy_calibration_view_test.cpp`
- Modify: `src/ui/desktop/calibration/BUILD.bazel`

**Interfaces:**
- Consumes: `CalibrationSession` (6m-1), `LegacyDefinitionAdapter::project_definition` (Task 1), `FileActions::RomInfoEnum`.
- Produces, in namespace `fastecu::ui`:

```cpp
struct LegacyCalibrationView
{
    std::unique_ptr<FileActions::EcuCalDefStructure> view;
    // First map-decode failure, if any; that map keeps its placeholder text.
    std::optional<Error> decode_error;
};
Result<LegacyCalibrationView> project_legacy_calibration(const calibration::CalibrationSession& session);
```

The projection reproduces the slot that `MainWindow` pre-filled and `FileActions::open_subaru_rom_file` completed:
1. `RomInfo` starts as 16 × `" "`.
2. With a definition:
   - `project_definition`.
   - Strip a leading `0x` from `RomInfo[InternalIdAddress]`, `AddressList`, `XScaleAddressList` and `YScaleAddressList` (legacy `normalize_definition_addresses`).
   - `RomInfo[FlashMethod] = protocol.flash_method`.
3. Without one, `RomInfo[FlashMethod]` is set only when the flash method is non-empty (ECU read).
4. `RomInfo[ChecksumModule]` is set only when `protocol.checksum_module` is non-empty.
5. `RomInfo[FileSize] = file_size_label` and `FileSize = unpadded_size`.
6. `RomId`, `McuType`, `Kernel`, `KernelStartAddr` come from the protocol info. `FlashMethod` (the field, which nothing reads) is set for an ECU read only.
7. `FileName`/`FullFileName` come from the source, `FullRomData` is the session bytes, and `OemEcuFile = true`.
8. Each map's `MapData`/`XScaleData`/`YScaleData` comes from `session.decode_map(i)`. A failing map keeps its placeholder, and the first error is reported.

- [ ] **Step 1: Write the failing tests**

Create `src/ui/desktop/calibration/legacy_calibration_view_test.cpp`. Each test runs the same input through legacy `FileActions::open_subaru_rom_file` (with `MainWindow`'s pre-fill) and through `RomOpenUseCase` + `project_legacy_calibration`, then compares.

```cpp
#include "src/ui/desktop/calibration/legacy_calibration_view.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/calibration/session/rom_open.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::ui
{
namespace
{

using fastecu::testing::IsOk;

// "TESTROM" at 0x10, one uint8 map byte 0x2A at 0x20; synthetic.
std::vector<std::uint8_t> synthetic_rom(std::size_t size = 64)
{
    std::vector<std::uint8_t> rom(size, 0);
    const std::string id = "TESTROM";
    std::ranges::copy(id, rom.begin() + 0x10);
    rom[0x20] = 0x2A;
    return rom;
}

constexpr std::string_view kDefinition = R"xml(
<rom>
  <romid><xmlid>TESTROM</xmlid><internalidaddress>10</internalidaddress>
    <internalidstring>TESTROM</internalidstring><make>Subaru</make>
    <flashmethod>alias_a</flashmethod><checksummodule>def-module</checksummodule></romid>
  <scaling name="Raw" toexpr="x" frexpr="x" format="%d" storagetype="uint8" endian="big"/>
  <table name="Idle" address="20" type="1D" category="Idle" scaling="Raw" storagetype="uint8"/>
</rom>)xml";

class LegacyCalibrationView : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg.initialize(), IsOk());
    }

    void enable_ecuflash_definitions()
    {
        cfg.put("/defs/test.xml", kDefinition);
        cfg.file_system.files["/defs/test.xml"] = {};
        cfg.file_system.directory_entries["/defs"] = {{.name = "test.xml", .is_directory = false}};
        auto& settings = cfg.session.settings();
        settings.primary_definition_base = "ecuflash";
        settings.use_ecuflash_definitions = "enabled";
        settings.ecuflash_definition_files_directory = "/defs";
        file_actions.create_ecuflash_def_id_list();
    }

    // What MainWindow::open_calibration_file left in its slot.
    FileActions::EcuCalDefStructure legacy_open(const QString& path)
    {
        FileActions::EcuCalDefStructure slot;
        while (slot.RomInfo.length() < slot.RomInfoStrings.length())
        {
            slot.RomInfo.append(" ");
        }
        EXPECT_NE(file_actions.open_subaru_rom_file(&slot, path), nullptr);
        return slot;
    }

    FileActions::EcuCalDefStructure projected(const calibration::RomOpenOutcome& outcome)
    {
        const calibration::CalibrationSession session(calibration::SessionId{1}, outcome.contents);
        auto view = project_legacy_calibration(session);
        EXPECT_THAT(view, IsOk());
        EXPECT_FALSE(view->decode_error.has_value());
        return *view->view;
    }

    config::testing::ConfigSessionFixture cfg;
    InMemoryAtomicFileWriter writer;
    FileActions file_actions{cfg.file_system, cfg.resource_bundle, cfg.file_repository, writer, cfg.events,
                             cfg.session};
    definition::DefinitionService definitions{cfg.file_system, cfg.file_repository, writer};
    calibration::RomOpenUseCase opener{file_actions,  definitions, cfg.file_repository, cfg.file_system,
                                       cfg.events,    cfg.session};
};

TEST_F(LegacyCalibrationView, FileWithoutDefinitionMatchesLegacyOpen)
{
    cfg.file_repository.files["/cal/a.bin"] = synthetic_rom();

    const FileActions::EcuCalDefStructure legacy = legacy_open("/cal/a.bin");
    ASSERT_THAT(cfg.session.select_row(0), IsOk());
    const auto outcome = opener.open_file("/cal/a.bin");
    ASSERT_THAT(outcome, IsOk());

    EXPECT_EQ(projected(*outcome), legacy);
}

TEST_F(LegacyCalibrationView, EcuFlashDefinitionMatchesLegacyOpen)
{
    enable_ecuflash_definitions();
    cfg.file_repository.files["/cal/a.bin"] = synthetic_rom();

    const FileActions::EcuCalDefStructure legacy = legacy_open("/cal/a.bin");
    ASSERT_TRUE(legacy.use_ecuflash_definition);
    ASSERT_THAT(cfg.session.select_row(0), IsOk());
    const auto outcome = opener.open_file("/cal/a.bin");
    ASSERT_THAT(outcome, IsOk());

    const FileActions::EcuCalDefStructure view = projected(*outcome);
    EXPECT_EQ(view, legacy);
    EXPECT_EQ(view.MapData, QStringList{"42,"});
    EXPECT_EQ(view.RomInfo.at(FileActions::InternalIdAddress), QString("10")); // 0x stripped
}

TEST_F(LegacyCalibrationView, EcuReadMatchesLegacyHandoff)
{
    enable_ecuflash_definitions();
    const std::string filename = "TESTROM2026-09-28_10h00m00s.bin";

    // MainWindow's read branch: pre-filled slot, protocol fields, then open.
    FileActions::EcuCalDefStructure legacy;
    while (legacy.RomInfo.length() < legacy.RomInfoStrings.length())
    {
        legacy.RomInfo.append(" ");
    }
    legacy.RomInfo.replace(FileActions::FlashMethod, "proto_b");
    legacy.FlashMethod = "proto_b";
    legacy.Kernel = "/kernels/b.bin";
    legacy.KernelStartAddr = "0x0";
    legacy.McuType = "M32R";
    const std::vector<std::uint8_t> rom = synthetic_rom();
    legacy.FullRomData = QByteArray(reinterpret_cast<const char *>(rom.data()), static_cast<qsizetype>(rom.size()));
    legacy.RomId = "TESTROM";
    legacy.FileName = QString::fromStdString(filename);
    ASSERT_NE(file_actions.open_subaru_rom_file(&legacy, legacy.FileName), nullptr);

    ASSERT_THAT(cfg.session.select_row(0), IsOk());
    const auto outcome = opener.adopt_read_image(calibration::ReadImage{
        .rom = rom,
        .filename = filename,
        .rom_id = "TESTROM",
        .protocol_name = "proto_b",
        .kernel_path = "/kernels/b.bin",
        .kernel_start_address = "0x0",
    });
    ASSERT_THAT(outcome, IsOk());

    FileActions::EcuCalDefStructure view = projected(*outcome);
    // The FlashMethod *field* is write-only in the UI: legacy kept the
    // protocol selected before the read, the session keeps the resolved one.
    view.FlashMethod = legacy.FlashMethod;
    EXPECT_EQ(view, legacy);
}

TEST_F(LegacyCalibrationView, SizeRejectedDefinitionShowsNoMaps)
{
    enable_ecuflash_definitions();
    cfg.file_repository.files["/cal/short.bin"] = synthetic_rom(0x20);

    const FileActions::EcuCalDefStructure legacy = legacy_open("/cal/short.bin");
    ASSERT_THAT(cfg.session.select_row(0), IsOk());
    const auto outcome = opener.open_file("/cal/short.bin");
    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->size_rejected);

    const FileActions::EcuCalDefStructure view = projected(*outcome);
    // Legacy cleared only NameList; every consumer iterates NameList, so an
    // empty map set is the same presentation.
    EXPECT_TRUE(legacy.NameList.isEmpty());
    EXPECT_TRUE(view.NameList.isEmpty());
    EXPECT_EQ(view.RomInfo, legacy.RomInfo);
    EXPECT_EQ(view.FullRomData, legacy.FullRomData);
    EXPECT_EQ(view.use_ecuflash_definition, legacy.use_ecuflash_definition);
}

} // namespace
} // namespace fastecu::ui
```

Add to `src/ui/desktop/calibration/BUILD.bazel`:

```python
qt_cc_library(
    name = "legacy_calibration_view",
    srcs = ["legacy_calibration_view.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["legacy_calibration_view.h"],
    deps = QT_DEPS + [
        "//src/backend/calibration/session:calibration_session",
        "//src/backend/definition/legacy:legacy_definition_adapter",
        "//src/backend/definitions",
        "//src/backend/ports",
    ],
)

fastecu_gtest(
    name = "legacy_calibration_view_test",
    srcs = ["legacy_calibration_view_test.cpp"],
    deps = [
        ":legacy_calibration_view",
        "//src/backend/calibration/session:rom_open",
        "//src/backend/config/testing:config_session_fixture",
        "//src/backend/definition:definition_service",
        "//src/backend/ports/testing:in_memory_atomic_file_writer",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

If `//src/backend/definitions` or `//src/backend/definition/legacy:legacy_definition_adapter` is not visible to `//src/ui/desktop/calibration`, check their `visibility`. Both packages grant `//bazel/qt:qt_layer`, which includes `//src/ui/...`. No ratchet entry may be added.

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/ui/desktop/calibration:legacy_calibration_view_test`
Expected: FAIL, because `legacy_calibration_view.h` does not exist.

- [ ] **Step 3: Implement**

Create `src/ui/desktop/calibration/legacy_calibration_view.h`:

```cpp
#pragma once

#include <memory>
#include <optional>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"

namespace fastecu::ui
{

// Transitional (step 6m-2, removed in 6m-3). The legacy struct every desktop
// calibration reader and writer still takes, built once from an opened
// session. It is what FileActions::open_subaru_rom_file left in MainWindow's
// slot, and until 6m-3 it is the only byte store the UI reads or writes.
struct LegacyCalibrationView
{
    std::unique_ptr<FileActions::EcuCalDefStructure> view;
    // First map-decode failure; that map keeps its placeholder text, as legacy.
    std::optional<Error> decode_error;
};

Result<LegacyCalibrationView> project_legacy_calibration(const calibration::CalibrationSession& session);

} // namespace fastecu::ui
```

Create `src/ui/desktop/calibration/legacy_calibration_view.cpp`:

```cpp
#include "src/ui/desktop/calibration/legacy_calibration_view.h"

#include <cstddef>
#include <utility>

#include "src/backend/definition/legacy/legacy_definition_adapter.h"

namespace fastecu::ui
{
namespace
{

QString qs(const std::string& text)
{
    return QString::fromStdString(text);
}

// FileActions::normalize_definition_addresses.
void strip_hex_prefix(QString& address)
{
    if (address.startsWith("0x"))
    {
        address.remove(0, 2);
    }
}

} // namespace

Result<LegacyCalibrationView> project_legacy_calibration(const calibration::CalibrationSession& session)
{
    auto view = std::make_unique<FileActions::EcuCalDefStructure>();
    FileActions::EcuCalDefStructure& legacy = *view;
    // MainWindow's pre-fill of a fresh slot.
    while (legacy.RomInfo.length() < legacy.RomInfoStrings.length())
    {
        legacy.RomInfo.append(" ");
    }

    const calibration::RomProtocolInfo& protocol = session.protocol();
    if (const calibration::ResolvedDefinition *definition = session.definition(); definition != nullptr)
    {
        if (Status projected = definition::LegacyDefinitionAdapter::project_definition(
                legacy, definition->definition, definition->format);
            !projected.has_value())
        {
            return std::unexpected(projected.error());
        }
        strip_hex_prefix(legacy.RomInfo[FileActions::InternalIdAddress]);
        for (QStringList *addresses : {&legacy.AddressList, &legacy.XScaleAddressList, &legacy.YScaleAddressList})
        {
            for (QString& address : *addresses)
            {
                strip_hex_prefix(address);
            }
        }
        legacy.RomInfo[FileActions::FlashMethod] = qs(protocol.flash_method);
    }
    else if (!protocol.flash_method.empty())
    {
        legacy.RomInfo[FileActions::FlashMethod] = qs(protocol.flash_method);
    }
    if (!protocol.checksum_module.empty())
    {
        legacy.RomInfo[FileActions::ChecksumModule] = qs(protocol.checksum_module);
    }
    legacy.RomInfo[FileActions::FileSize] = qs(protocol.file_size_label);
    legacy.FileSize = QString::number(static_cast<qulonglong>(protocol.unpadded_size));
    legacy.RomId = qs(protocol.rom_id);
    legacy.McuType = qs(protocol.mcu_type);
    legacy.Kernel = qs(protocol.kernel_path);
    legacy.KernelStartAddr = qs(protocol.kernel_start_address);
    if (session.source().origin == calibration::RomOrigin::EcuRead)
    {
        legacy.FlashMethod = qs(protocol.flash_method);
    }
    legacy.FileName = qs(session.source().display_name);
    legacy.FullFileName = qs(session.source().path);
    legacy.FullRomData = QByteArray(reinterpret_cast<const char *>(session.rom().data()),
                                    static_cast<qsizetype>(session.rom().size()));
    legacy.OemEcuFile = true;

    LegacyCalibrationView result{.view = std::move(view)};
    for (qsizetype index = 0; index < legacy.MapData.size(); ++index)
    {
        auto decoded = session.decode_map(static_cast<std::size_t>(index));
        if (!decoded.has_value())
        {
            if (!result.decode_error.has_value())
            {
                result.decode_error = decoded.error();
            }
            continue;
        }
        legacy.MapData.replace(index, qs(decoded->map_data));
        legacy.XScaleData.replace(index, qs(decoded->x_axis_data));
        legacy.YScaleData.replace(index, qs(decoded->y_axis_data));
    }
    return result;
}

} // namespace fastecu::ui
```

- [ ] **Step 4: Run to verify they pass**

Run: `bazel test --config=release //src/ui/desktop/calibration:legacy_calibration_view_test`
Expected: PASS. If an equality fails, gtest's printer prints no field names for this struct. Bisect by comparing individual members (`RomInfo`, `MapData`, `AddressList`, `FileSize`, …), then fix the **projection**, never the legacy side. Record any genuinely irreconcilable field as a ledgered ruling, as done for `FlashMethod`.

- [ ] **Step 5: Mutation check**

Remove the `strip_hex_prefix` calls temporarily. `EcuFlashDefinitionMatchesLegacyOpen` must fail. Revert.

- [ ] **Step 6: Commit**

```bash
git add src/ui/desktop/calibration
git commit -m "feat(ui): project a calibration session onto the legacy view (6m-2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 5: Services carry the workspace

**Files:**
- Modify: `src/ui/desktop/main_window_services.h`, `apps/desktop/desktop_composition.cpp` (`services()`), `apps/desktop/desktop_composition_test.cpp`

**Interfaces:**
- Produces: `MainWindowServices::calibrations`, of type `fastecu::calibration::CalibrationWorkspace&`, placed after `file_actions`.

- [ ] **Step 1: Write the failing assertion**

In `desktop_composition_test.cpp`'s `servicesReferToTheCompositionsOwnObjects`, next to the existing identity checks, add:

```cpp
        QCOMPARE(&first.calibrations, composition.calibration_workspace_.get());
        QCOMPARE(&second.calibrations, &first.calibrations);
```

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test --config=release //apps/desktop:desktop_composition_test`
Expected: FAIL to compile, because `MainWindowServices` has no member `calibrations`.

- [ ] **Step 3: Implement**

`main_window_services.h`: add the forward declaration

```cpp
namespace fastecu::calibration
{
class CalibrationWorkspace;
}
```

Then add the field after `file_actions`:

```cpp
    fastecu::calibration::CalibrationWorkspace& calibrations;
```

`desktop_composition.cpp` `services()`: add `.calibrations = *calibration_workspace_,` after `.file_actions`.

`src/ui/desktop/mainwindow_test.cpp` `TestServices` also builds `MainWindowServices`, so give it the same field now to keep the tree building. Add these members after `file_actions`, in this order:

```cpp
    fastecu::definition::DefinitionService definition_service{file_system, file_repository, file_writer};
    fastecu::calibration::RomOpenUseCase rom_open{file_actions, definition_service, file_repository,
                                                  file_system,  events,             config};
    fastecu::calibration::CalibrationWorkspace calibrations{rom_open};
```

Add `.calibrations = calibrations,` in its `services()`. Add the includes `src/backend/calibration/session/calibration_workspace.h`, `src/backend/calibration/session/rom_open.h` and `src/backend/definition/definition_service.h`. Add the deps `//src/backend/calibration/session:calibration_workspace`, `//src/backend/calibration/session:rom_open` and `//src/backend/definition:definition_service` to `test_mainwindow`.

- [ ] **Step 4: Run to verify**

Run: `bazel test --config=release //apps/desktop:desktop_composition_test`
Expected: PASS. `//src/ui/desktop:test_mainwindow` still fails to build until Task 6, and that is expected in this intermediate commit.

- [ ] **Step 5: Commit**

```bash
git add src/ui/desktop/main_window_services.h apps/desktop src/ui/desktop/mainwindow_test.cpp src/ui/desktop/BUILD.bazel
git commit -m "feat(desktop): pass the calibration workspace to the main window (6m-2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 6: `MainWindow` owns sessions and addresses them by ID

**Files:**
- Modify: `src/ui/desktop/mainwindow.h`, `mainwindow.cpp`, `menu_actions.cpp`, `calibration_maps.{h,cpp}`, `calibration_treewidget.{h,cpp}`, `src/ui/desktop/BUILD.bazel` (`desktop` deps)
- Test: `src/ui/desktop/mainwindow_test.cpp`

**Interfaces:**
- Consumes: `CalibrationWorkspace` (services), `project_legacy_calibration` (Task 4), `session_key_text`/`parse_session_key`/`MapWindowId::session` (Task 2), `HexEdit(data, name)` (Task 3).
- Produces (private to `MainWindow`):

```cpp
    struct OpenCalibration
    {
        fastecu::calibration::SessionId id;
        std::unique_ptr<FileActions::EcuCalDefStructure> legacy;
    };
    std::vector<OpenCalibration> calibrations_; // files-tree order
    fastecu::calibration::CalibrationWorkspace *calibrationWorkspace = nullptr;

    FileActions::EcuCalDefStructure *legacy_calibration(fastecu::calibration::SessionId id);
    std::optional<fastecu::calibration::SessionId> session_of(const QTreeWidgetItem *files_item) const;
    FileActions::EcuCalDefStructure *selected_legacy_calibration();
    QTreeWidgetItem *files_tree_item(fastecu::calibration::SessionId id) const;
    bool add_calibration(fastecu::calibration::SessionId id);
    void update_protocol_info(const QString& flash_method); // was (int rom_number)
```

- [ ] **Step 1: Teach `ModalDriver` the missing-definition prompt**

In `mainwindow_test.cpp`'s `ModalDriver::drive()`, before the `if (elapsed_.elapsed() > 3000)` block, add:

```cpp
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (dialog == nullptr || !dialog->isVisible())
            {
                continue;
            }
            for (QRadioButton *button : dialog->findChildren<QRadioButton *>())
            {
                if (button->text() == kContinueWithoutDefinitionText)
                {
                    ++missing_definition_prompt_count_;
                    dialog->reject(); // the continue-without path
                    return;
                }
            }
        }
```

Add `constexpr auto kContinueWithoutDefinitionText = "Continue without definition file";` beside the other constants, and `#include <QRadioButton>`. Add the counter `int missing_definition_prompt_count_ = 0;` and the accessor `int missingDefinitionPromptCount() const { return missing_definition_prompt_count_; }`.

Add a test helper after `startEcuOperations`:

```cpp
// A synthetic image on disk for the open-file path.
QString writeRom(const QTemporaryDir& dir, const QString& name, char fill, int size = 64)
{
    const QString path = dir.filePath(name);
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly) || file.write(QByteArray(size, fill)) != size)
    {
        return {};
    }
    return path;
}
```

- [ ] **Step 2: Write the failing tests and adapt the slot-based ones**

Adapt the existing tests:
- **`handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling` (line ~787):** replace `QVERIFY(window.ecuCalDef[window.ecuCalDefIndex] == nullptr);` with

```cpp
        QVERIFY(window.calibrations_.empty());
        QVERIFY(services.calibrations.ids().empty());
```

- **`readOfAnUnsupportedProtocolReleasesTheReadSlot`:** rename to `readOfAnUnsupportedProtocolAddsNoCalibration`. Replace the `slot` bookkeeping with:

```cpp
        QCOMPARE(window.calibrations_.size(), std::size_t{0});
        ...
        QCOMPARE(window.calibrations_.size(), std::size_t{0});
        QVERIFY(services.calibrations.ids().empty());
        QCOMPARE(window.ui->calibrationFilesTreeWidget->topLevelItemCount(), 0);
```

- **`cancellingTheChecksumWarningStopsVoltagePolling`:** replace the hand-built calibration with a real open, driven through the missing-definition prompt:

```cpp
        QTemporaryDir roms;
        const QString rom_path = writeRom(roms, "test.bin", '\x5a', 16);
        QVERIFY(!rom_path.isEmpty());
        ModalDriver open_driver{QString()};
        open_driver.start();
        QCOMPARE(window.open_calibration_file(rom_path), 0);
        open_driver.stop();
        QCOMPARE(open_driver.missingDefinitionPromptCount(), 1);
        selectSubaruProtocol(window, "sub_ecu_denso_sh7058_can_checksum_na");
```

  Opening first and selecting the protocol afterwards keeps the protocol under test. Delete the `window.ecuCalDef[0] = ...` lines, and replace the final byte assertion with

```cpp
        QCOMPARE(window.calibrations_.front().legacy->FullRomData, QByteArray(16, '\x5a'));
```

- **`romFlashMethodSelectsTheLastMatchingRow` / `unmatchedRomFlashMethodChangesNothing`:** drop the hand-built calibration and call `window.update_protocol_info("sub_ecu_denso_sh7058");` and `window.update_protocol_info("no_such_protocol");` respectively.

Add the new slots, beside the calibration tests:

```cpp
    void definitionlessOpenPromptsOnceAndAppliesPlaceholders()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        QTemporaryDir roms;
        const QString path = writeRom(roms, "a.bin", '\x11');

        ModalDriver driver{QString()};
        driver.start();
        QCOMPARE(window.open_calibration_file(path), 0);
        driver.stop();

        QVERIFY(!driver.timedOut());
        QCOMPARE(driver.missingDefinitionPromptCount(), 1);
        QCOMPARE(window.calibrations_.size(), std::size_t{1});
        const auto& legacy = *window.calibrations_.front().legacy;
        QCOMPARE(legacy.RomInfo.at(FileActions::XmlId), QString("UnknownID"));
        QCOMPARE(legacy.FileName, QString("a.bin"));
        QCOMPARE(services.calibrations.ids().size(), std::size_t{1});
        QCOMPARE(window.ui->calibrationFilesTreeWidget->topLevelItem(0)->text(2),
                 fastecu::ui::session_key_text(window.calibrations_.front().id));
    }

    void closingAMiddleRomKeepsLaterRomsAddressable()
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
        QCOMPARE(window.open_calibration_file(writeRom(roms, "c.bin", '\x0c')), 0);
        driver.stop();
        QCOMPARE(window.calibrations_.size(), std::size_t{3});
        const auto a = window.calibrations_.at(0).id;
        const auto c = window.calibrations_.at(2).id;
        QTreeWidget *files = window.ui->calibrationFilesTreeWidget;
        const QString c_key = files->topLevelItem(2)->text(2);

        for (int i = 0; i < files->topLevelItemCount(); ++i)
        {
            files->topLevelItem(i)->setSelected(i == 1);
        }
        window.close_calibration();

        QCOMPARE(window.calibrations_.size(), std::size_t{2});
        QCOMPARE(services.calibrations.ids(), (std::vector{a, c}));
        QCOMPARE(files->topLevelItemCount(), 2);
        QCOMPARE(files->topLevelItem(1)->text(2), c_key); // not renumbered
        QVERIFY(window.legacy_calibration(c) != nullptr);
        QCOMPARE(window.legacy_calibration(c)->FileName, QString("c.bin"));
        QCOMPARE(window.legacy_calibration(c)->FullRomData.at(0), '\x0c');
    }

    void windowsOfAClosedRomAreInert()
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
        const auto b = window.calibrations_.at(1).id;
        window.close_calibration(); // b is selected after its open
        QVERIFY(window.legacy_calibration(b) == nullptr);

        const QString stale = fastecu::ui::session_key_text(b) + ",0,Idle";
        auto *content = new QWidget;
        QMdiSubWindow *sub = window.ui->mdiArea->addSubWindow(content);
        sub->setObjectName(stale);
        content->setObjectName(stale);
        window.ui->mdiArea->setActiveSubWindow(sub);
        QObject destroyed_window;
        destroyed_window.setObjectName(stale);

        window.set_maptablewidget_items();
        window.selectable_combobox_item_changed("anything");
        window.checkbox_state_changed(2);
        window.close_calibration_map(&destroyed_window);

        QCOMPARE(window.calibrations_.size(), std::size_t{1});
        QCOMPARE(window.ui->calibrationFilesTreeWidget->topLevelItemCount(), 1);
    }

    void hexEditorOutlivesItsRom()
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
        driver.stop();

        window.show_hex_editor();
        window.close_calibration();

        QCOMPARE(window.findChildren<HexEdit *>().size(), qsizetype{1});
        QVERIFY(window.calibrations_.empty());
    }
```

Add includes `src/ui/desktop/calibration/session_key.h` and `src/ui/desktop/hexedit/hexedit.h`, plus `<QMdiArea>`, `<QMdiSubWindow>`, `<QTreeWidget>` and `<QTemporaryDir>` if missing. Add the dep `//src/ui/desktop/calibration:session_key` to `test_mainwindow`. Check how the file exposes `MainWindow` privates (`ui`, `calibrations_`, private slots); it already reads `window.ecuCalDef`, `window.ui` and `window.vbatt_timer`, so the new members are reachable the same way.

- [ ] **Step 3: Run to verify they fail**

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow`
Expected: FAIL to compile, because `calibrations_`, `legacy_calibration` and `update_protocol_info(QString)` do not exist.

- [ ] **Step 4: Replace ownership in `mainwindow.h`**

- Delete `int ecuCalDefIndex = 0;`, `std::array<FileActions::EcuCalDefStructure *, 100> ecuCalDef{};` and the `ecuCalDefTemp` comment.
- Add the members and helpers from this task's Interfaces block, with `#include <memory>`, `#include <optional>`, `#include <vector>` and `#include "src/backend/calibration/session/calibration_workspace.h"`.
- Change `void update_protocol_info(int rom_number);` to `void update_protocol_info(const QString& flash_method);`.

In the constructor, next to `configSession = &services_.config;`, add `calibrationWorkspace = &services_.calibrations;`. At line ~486, change `if (ecuCalDefIndex > 0)` to `if (!calibrations_.empty())`.

- [ ] **Step 5: Add the helpers (`mainwindow.cpp`, beside `update_protocol_info`)**

```cpp
FileActions::EcuCalDefStructure *MainWindow::legacy_calibration(fastecu::calibration::SessionId id)
{
    const auto found =
        std::ranges::find_if(calibrations_, [id](const OpenCalibration& open) { return open.id == id; });
    return found == calibrations_.end() ? nullptr : found->legacy.get();
}

std::optional<fastecu::calibration::SessionId> MainWindow::session_of(const QTreeWidgetItem *files_item) const
{
    return files_item == nullptr ? std::nullopt : fastecu::ui::parse_session_key(files_item->text(2));
}

FileActions::EcuCalDefStructure *MainWindow::selected_legacy_calibration()
{
    const QList<QTreeWidgetItem *> selected = ui->calibrationFilesTreeWidget->selectedItems();
    const auto id = selected.isEmpty() ? std::nullopt : session_of(selected.at(0));
    return id.has_value() ? legacy_calibration(*id) : nullptr;
}

QTreeWidgetItem *MainWindow::files_tree_item(fastecu::calibration::SessionId id) const
{
    for (int i = 0; i < ui->calibrationFilesTreeWidget->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem *item = ui->calibrationFilesTreeWidget->topLevelItem(i);
        if (session_of(item) == id)
        {
            return item;
        }
    }
    return nullptr;
}

// Presents a session the workspace just opened: its one legacy view, the
// protocol refresh, the missing-definition prompt, and both trees -- in the
// order the legacy slot path ran them.
bool MainWindow::add_calibration(fastecu::calibration::SessionId id)
{
    const fastecu::calibration::CalibrationSession *session = calibrationWorkspace->find(id);
    if (session == nullptr)
    {
        return false;
    }
    auto projected = fastecu::ui::project_legacy_calibration(*session);
    if (!projected.has_value())
    {
        emit LOG_E("Unable to present calibration file [" + QString(fastecu::to_string(projected.error().kind)) +
                       "]: " + qs(projected.error().detail),
                   true, true);
        (void)calibrationWorkspace->close(id);
        return false;
    }
    if (projected->decode_error.has_value())
    {
        emit LOG_E("Error decoding calibration map values [" +
                       QString(fastecu::to_string(projected->decode_error->kind)) +
                       "]: " + qs(projected->decode_error->detail),
                   true, true);
    }
    FileActions::EcuCalDefStructure *legacy = projected->view.get();
    calibrations_.push_back(OpenCalibration{.id = id, .legacy = std::move(projected->view)});

    update_protocol_info(legacy->RomInfo.at(fileActions->FlashMethod));
    if (!legacy->use_romraider_definition && !legacy->use_ecuflash_definition)
    {
        prompt_for_missing_definition(legacy);
    }
    calibrationTreeWidget->buildCalibrationFilesTree(id, ui->calibrationFilesTreeWidget, legacy);
    calibrationTreeWidget->buildCalibrationDataTree(ui->calibrationDataTreeWidget, legacy);
    return true;
}
```

Change `update_protocol_info` to take the flash method directly:

```cpp
void MainWindow::update_protocol_info(const QString& flash_method)
{
    emit LOG_D("Update protocol info by selected ROM with FlashMethod: " + flash_method, true, true);
    // The last matching row wins, as the legacy scan did; no match changes
    // nothing.
    if (const bool info_updated = configSession->select_by_protocol_name(flash_method.toStdString()); info_updated)
    {
        emit LOG_D("Protocol info for selected ROM updated", true, true);
    }
    else
    {
        emit LOG_D("Could not find protocol for selected ROM!", true, true);
    }
    status_bar_ecu_label->setText(protocol_field(selected_vehicle(), &ProtocolEntry::description) + " ");
}
```

Add includes `src/ui/desktop/calibration/legacy_calibration_view.h` and `src/ui/desktop/calibration/session_key.h`. In `src/ui/desktop/BUILD.bazel` `desktop` deps, add `"//src/backend/calibration/session:calibration_workspace"`, `"//src/ui/desktop/calibration:legacy_calibration_view"` and `"//src/ui/desktop/calibration:session_key"`.

- [ ] **Step 6: Open, read handoff, and close**

`open_calibration_file`: keep the file dialog. Replace everything from `ecuCalDef[ecuCalDefIndex] = new ...` to the end with:

```cpp
    const auto opened = calibrationWorkspace->open_file(filename.toStdString());
    if (!opened.has_value())
    {
        return 1;
    }
    return add_calibration(opened->id) ? 0 : 1;
```

The use case already raised the legacy notice through the shared `QtEventSink`.

`start_ecu_operations`:
- Declare `FileActions::EcuCalDefStructure *legacy = nullptr;` and read-branch locals `QString read_kernel_path, read_kernel_address, read_mcu;` in place of `int rom_number = 0;`, `release_read_slot` and its comment.
- **Write branch:**
  - Resolve the selection with `legacy = selected_legacy_calibration();` in place of `rom_number = indexOfTopLevelItem(...)`.
  - Keep the "No file selected!" path; it now also covers a selected row whose session is gone.
  - Replace every `ecuCalDef[rom_number]->` with `legacy->`.
  - `update_protocol_info(rom_number)` becomes `update_protocol_info(legacy->RomInfo.at(fileActions->FlashMethod))`.
  - `runChecksumCorrection(ecuCalDef[rom_number])` becomes `runChecksumCorrection(legacy)`.
- **Read branch:** replace the slot allocation with:

```cpp
            // Nothing is allocated before the read: the image is adopted into
            // a session only after it succeeded.
            update_protocol_info(qs(selected_vehicle().protocol_name));
            read_kernel_path = QString::fromStdString(fastecu::flash::kernel_path(
                kernel_dir.toStdString(),
                fastecu::config::protocol_field_or_placeholder(selected_vehicle(), &ProtocolEntry::kernel)));
            read_kernel_address = protocol_field(selected_vehicle(), &ProtocolEntry::kernel_addr);
            read_mcu = protocol_field(selected_vehicle(), &ProtocolEntry::mcu);
```

- **`controller.run({...})`** takes these fields:

```cpp
            .mcu = (legacy != nullptr ? legacy->McuType : read_mcu).toStdString(),
            .kernel_path = (legacy != nullptr ? legacy->Kernel : read_kernel_path).toStdString(),
            .image = fastecu::flash::portableImageForOperation(
                operation, legacy != nullptr ? bytes::view(legacy->FullRomData) : bytes::ByteView{}),
            .paths = configSession->effective_paths(),
            .display_filename = legacy != nullptr ? legacy->FileName.toStdString() : std::string{},
```

- **After the run:**
  - Delete the block that copied `read_bytes`/`rom_id` into the slot.
  - Replace the `else if (cmd_type == "read")` body with:

```cpp
        else if (cmd_type == "read")
        {
            if (outcome.status == fastecu::flash::FlashOperationStatus::Completed && outcome.read_bytes &&
                !outcome.read_bytes->empty())
            {
                const QString dateTimeString = QDateTime::currentDateTime().toString("yyyy-MM-dd_hh'h'mm'm'ss's'");
                const std::string rom_id = outcome.rom_id.value_or(std::string{});
                const auto opened = calibrationWorkspace->adopt_read_image(fastecu::calibration::ReadImage{
                    .rom = *outcome.read_bytes,
                    .filename = fastecu::flash::read_image_filename(rom_id, dateTimeString.toStdString()),
                    .rom_id = rom_id,
                    .protocol_name = selected_vehicle().protocol_name,
                    .kernel_path = read_kernel_path.toStdString(),
                    .kernel_start_address = read_kernel_address.toStdString(),
                });
                if (opened.has_value() && add_calibration(opened->id))
                {
                    save_calibration_file_as();
                }
            }
        }
        else if (legacy != nullptr)
        {
            legacy->FullRomData = fullRomDataTmp;
        }
```

  - Delete the trailing `if (release_read_slot) { delete ...; }`.

`close_calibration`: replace the lookup, the window scan's rom key and the renumbering:

```cpp
void MainWindow::close_calibration()
{
    const QList<QTreeWidgetItem *> selected = ui->calibrationFilesTreeWidget->selectedItems();
    const auto id = selected.isEmpty() ? std::nullopt : session_of(selected.at(0));
    if (!id.has_value())
    {
        return;
    }
    const int romNumber = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selected.at(0));
    const QString romKey = fastecu::ui::session_key_text(*id);

    for (int i = 0; i < ui->calibrationDataTreeWidget->topLevelItemCount(); i++)
    {
        for (int j = 0; j < ui->calibrationDataTreeWidget->topLevelItem(i)->childCount(); j++)
        {
            for (int k = 0; k < ui->mdiArea->subWindowList().count(); k++)
            {
                int mapNumber = ui->calibrationDataTreeWidget->topLevelItem(i)->child(j)->text(1).toInt();
                QString mapName = ui->calibrationDataTreeWidget->topLevelItem(i)->child(j)->text(0);

                QString calMapWindowName = romKey + "," + QString::number(mapNumber) + "," + mapName;
                QMdiSubWindow *w = ui->mdiArea->subWindowList().at(k);
                if (w->objectName().startsWith(calMapWindowName))
                {
                    ui->mdiArea->removeSubWindow(w);
                }
            }
        }
    }
    delete ui->calibrationFilesTreeWidget->takeTopLevelItem(romNumber);
    std::erase_if(calibrations_, [&id](const OpenCalibration& open) { return open.id == *id; });
    (void)calibrationWorkspace->close(*id);

    // (unchanged from here: reselect the previous row or clear the data tree)
```

Keep the remainder of the function (from `if (ui->calibrationFilesTreeWidget->topLevelItemCount() > 0)`) exactly as it is.

`close_calibration_map`:

```cpp
void MainWindow::close_calibration_map(QObject *obj)
{
    const QStringList mapWindowString = obj->objectName().split(",");
    const auto id = mapWindowString.isEmpty() ? std::nullopt : fastecu::ui::parse_session_key(mapWindowString.at(0));
    QTreeWidgetItem *romItem = id.has_value() ? files_tree_item(*id) : nullptr;
    FileActions::EcuCalDefStructure *legacy = id.has_value() ? legacy_calibration(*id) : nullptr;
    if (romItem == nullptr || legacy == nullptr || mapWindowString.size() < 3)
    {
        return; // the ROM was closed before its window
    }
    const QString& mapName = mapWindowString.at(2);

    for (int i = 0; i < ui->calibrationFilesTreeWidget->topLevelItemCount(); i++)
    {
        ui->calibrationFilesTreeWidget->topLevelItem(i)->setSelected(false);
    }
    romItem->setSelected(true);
    const QModelIndex index = ui->calibrationFilesTreeWidget->selectionModel()->currentIndex();
    emit ui->calibrationFilesTreeWidget->clicked(index);

    QTreeWidgetItem *item;
    for (int i = 0; i < ui->calibrationDataTreeWidget->topLevelItemCount(); i++)
    {
        for (int j = 0; j < ui->calibrationDataTreeWidget->topLevelItem(i)->childCount(); j++)
        {
            item = ui->calibrationDataTreeWidget->topLevelItem(i)->child(j);
            if (item->text(0) == mapName)
            {
                item->setCheckState(0, Qt::Unchecked);

                for (int i_local = 0; i_local < legacy->NameList.count(); i_local++)
                {
                    if (legacy->NameList.at(i_local) == mapName)
                    {
                        legacy->VisibleList.replace(i_local, "0");
                    }
                }
            }
        }
    }
}
```

The legacy `if (mapRomNumber == romIndex)` guard is always true now: the selected row *is* `romItem`, whose key is `id`. That is why it disappears; do not reintroduce a positional comparison.

- [ ] **Step 7: The remaining sites (mechanical, by ID)**

For each function below, obtain the legacy view as stated. If it is `nullptr`, do exactly what the function already did when there was nothing to act on, which is return (or show the same message). Then replace `ecuCalDef[<n>]->` with `legacy->` and `ecuCalDef[<n>]` (a pointer argument) with `legacy`.

| Function | File | How the legacy view is obtained |
|---|---|---|
| `custom_menu_requested` | mainwindow.cpp | `if (calibrations_.empty())` replaces `ecuCalDefIndex == 0` |
| `save_calibration_file`, `save_calibration_file_as` | mainwindow.cpp | `legacy = selected_legacy_calibration();` The guard becomes `if (legacy == nullptr)` with the existing "No calibration to save!" message. Drop the `rom_number` variable and the dead `!= nullptr` re-check. |
| `selectable_combobox_item_changed`, `checkbox_state_changed` | mainwindow.cpp | `const auto id = fastecu::ui::parse_map_window_id(w); legacy = id ? legacy_calibration(id->session) : nullptr;` `mapNumber`/`map_number` come from `id->map_number`. |
| `calibration_files_treewidget_item_selected` | mainwindow.cpp | `legacy = selected_legacy_calibration();` Then `buildCalibrationDataTree(..., legacy)` and `update_protocol_info(legacy->RomInfo.at(fileActions->FlashMethod))`. |
| `calibration_data_treewidget_item_selected` | mainwindow.cpp | `legacy = selected_legacy_calibration();` and `const auto session = session_of(selectedFilesTreeItem);` (return if either is missing). The window-name prefix `QString::number(romIndex)` becomes `fastecu::ui::session_key_text(*session)`, and `new CalibrationMaps(legacy, *session, i, ...)`. Remove `romNumber`/`romIndex`. |
| `calibration_data_treewidget_item_expanded` / `_collapsed` | mainwindow.cpp | `legacy = selected_legacy_calibration();` |
| `inc_dec_value`, `set_value`, `interpolate_value`, `paste_value` | menu_actions.cpp | After `parse_map_window_id`: `FileActions::EcuCalDefStructure *legacy = legacy_calibration(id->session); if (legacy == nullptr) { return; }`. `ecuCalDef[id->rom_number]` becomes `legacy`. |
| `show_hex_editor` | menu_actions.cpp | `legacy = selected_legacy_calibration();` if null, return; else `new HexEdit(legacy->FullRomData, legacy->FileName, this);` |
| `set_maptablewidget_items` | menu_actions.cpp | `const auto id = fastecu::ui::parse_map_window_id(w);` `legacy = id ? legacy_calibration(id->session) : nullptr;` `mapNumber = id->map_number;` `mapName = w->objectName().split(",").value(2);` |

`CalibrationMaps`: change the constructor's `int romIndex` to `fastecu::calibration::SessionId session`, and build the name as `fastecu::ui::session_key_text(session) + "," + QString::number(mapIndex) + "," + ecuCalDef->NameList.at(mapIndex)`. Add the header include.

`CalibrationTreeWidget::buildCalibrationFilesTree`: change `int ecuCalDefIndex` to `fastecu::calibration::SessionId session`, and set column 2 with `topLevelFilesTreeItem->setText(2, fastecu::ui::session_key_text(session));`.

Then run `grep -n "ecuCalDef\[\|ecuCalDefIndex\|rom_number\]" src/ui/desktop/*.cpp src/ui/desktop/*.h`. Expected: no matches.

- [ ] **Step 8: Run to verify they pass**

Run: `bazel test --config=release //src/ui/desktop:all //src/ui/desktop/calibration:all //apps/desktop:all`
Expected: PASS, including the adapted and new `MainWindowTest` slots. Read `bazel-testlogs/src/ui/desktop/test_mainwindow/test.log` and confirm that each new slot is `PASS` and none timed out.

- [ ] **Step 9: Mutation check**

Temporarily reintroduce renumbering in `close_calibration`: after the erase, overwrite column 2 of the remaining rows with their positions. `closingAMiddleRomKeepsLaterRomsAddressable` must fail. Revert.

- [ ] **Step 10: Commit**

```bash
git add src/ui/desktop apps/desktop
git commit -m "feat(ui): own calibrations through the workspace and address them by session (6m-2)

MainWindow's 100-slot raw-pointer array and positional indices give way to
one legacy view per workspace session, keyed by SessionId in the files
tree and map-window names. Closing a ROM renumbers nothing and frees its
view; a failed or cancelled read allocates nothing.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 7: Documentation and gates

**Files:**
- Modify: `docs/modularization-plan.md` (section "6m — Definition and calibration sessions")

- [ ] **Step 1: Record progress**

Directly under the `### 6m — Definition and calibration sessions` heading, add:

```markdown
In progress. 6m-1 added the portable `CalibrationSession`, `RomOpenUseCase`
and `CalibrationWorkspace` (composition-owned, stable session IDs). 6m-2 moved
ownership and identity: `MainWindow` holds one legacy view per workspace
session, addressed by `SessionId` rather than tree position, and the post-read
handoff adopts an image only after a successful read. Rendering, every
mutation, and retirement of the legacy model remain for 6m-3; see the
[step 6m design](superpowers/specs/2026-09-28-step6m-calibration-sessions-design.md).
```

- [ ] **Step 2: Full gates**

```bash
bazel build -k --config=release //...
bazel test -k --config=release //...
prek run --all-files
bazel run --config=release //:clang_tidy_report_changed
```

Expected: all pass. Fix clang-tidy findings in touched files rather than suppressing them. A pre-existing finding in a touched file is converted, not `NOLINT`ed, as in 6m-1. If `//tests:serial_backend_tests` fails intermittently on Windows CI, that is the known flake: rerun it.

- [ ] **Step 3: Commit**

```bash
git add docs/modularization-plan.md
git commit -m "docs: record step 6m-1 and 6m-2 progress

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 4: Publish, after the user authorizes the push**

```bash
gh stack init --base master feat/6m-2-calibration-ownership
gh stack submit --auto --open
```

Then set the title `6m-2: Own calibrations through the workspace and address them by session`. Write a body in the repository's What/Why/Validation/References shape, ending with the Claude Code attribution line. Report CI as pending until it finishes.
