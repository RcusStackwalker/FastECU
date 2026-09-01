# Step 6a — De-widget `FileActions` — Design

**Status:** designed 2026-09-01, not started. The first slice of step 6 of the
[modularization plan](../../modularization-plan.md), deliberately scoped to run
**in parallel with the [per-family flash drain](2026-08-08-step5-tail-flash-drain-design.md)**
(waves 5-7, 14 families remaining) without contending for a single file.

**Goal:** remove Qt *UI* — widgets, dialogs, message boxes, and the `QObject`
signal surface — from `src/backend/definitions`, so that no code under
`src/backend/**` constructs or shows a user interface.

**Non-goal:** removing Qt *types*. `ConfigValuesStructure`,
`LogValuesStructure`, and `EcuCalDefStructure` stay `QString`/`QStringList`
typed and `//src/backend/definitions:definitions` stays a `qt_cc_library`.
Converting those models is later step-6 work that necessarily rewrites
drain-owned files, and is excluded here for exactly that reason.

## Why this slice is independent

Measured against `master` at `d69b0df` on 2026-09-01.

`FileActions` is named in about 85 files, but almost entirely as a **type
carrier**: `FileActions::EcuCalDefStructure`,
`FileActions::ConfigValuesStructure`, `FileActions::LogValuesStructure`,
`FileActions::RomInfoEnum` (chiefly `FlashMethod`), and the static
`FileActions::parse_nrc_message`, which alone has about 150 call sites. The
majority of those files are drain-owned — everything under
`src/platform/desktop/common/flash/legacy/` and `src/ui/desktop/flash/`, which
waves 5-7 delete family by family.

Filtering that frozen surface out of a full-tree grep leaves exactly one
reference in a drain-adjacent file:

```
src/platform/desktop/common/logging/logging_snapshot_adapter.cpp:88:
    if (!FileActions::validate_logger_values(log_values))
```

which is another static, in a file the drain does not own. **Nothing outside
`MainWindow` constructs `FileActions`, parents it, or connects its signals.**
So changing the class's nature — dropping `QWidget` and `Q_OBJECT` — is
invisible to every drain-owned file, provided the surface below stays frozen.

Independently confirmed on the other side: the wave commits (`ea633a4` and its
predecessors) touch `src/ui/desktop/mainwindow.{h,cpp}` only to delete a flash
dialog's include and its dispatch branch. They have never touched
`src/ui/desktop/menu_actions.cpp`, `src/ui/desktop/BUILD.bazel`,
`src/backend/definitions/**`, `src/backend/config/**`, `MODULE.bazel`, or
`.bazelrc`.

## The frozen surface

For the duration of this step, these keep their exact names, signatures, and
header path (`src/backend/definitions/file_actions.h`):

- `FileActions::ConfigValuesStructure`, `::LogValuesStructure`,
  `::EcuCalDefStructure` (the `using` aliases onto `config_values.h`,
  `log_values.h`, `ecu_cal_def.h`).
- `FileActions::RomInfoEnum` and all sixteen enumerators.
- Public data members `float_precision` and `protocolsStruct`, and the three
  struct instances `ConfigValuesStruct`, `LogValuesStruct`, `EcuCalDefStruct`.
- Statics `parse_nrc_message`, `parse_dtc_message`, `validate_flash_protocols`,
  `validate_logger_values`, `validate_logger_switches`,
  `validate_calibration_maps`, `collect_ecuflash_base_header_fields`,
  `collect_ecuflash_definition_body_lines`.

**No file under `src/platform/desktop/common/flash/legacy/` or
`src/ui/desktop/flash/` is edited by this step.** That is a completion
criterion, not an aspiration — see [Completion criteria](#completion-criteria).

## What changes

`FileActions` stops inheriting `QWidget`, drops `Q_OBJECT` and its four
`LOG_E` / `LOG_W` / `LOG_I` / `LOG_D` signals, and takes an injected
`fastecu::IEventSink&` as a fifth constructor parameter, replacing the trailing
`QWidget *parent`. The 38 `emit LOG_*` statements become `events_.log(...)`.

`tr()` is a `QObject` member function, so all 25 `tr(` sites must go. Twenty of
them sit inside message-box and dialog code that moves to the UI, where `tr()`
remains available; the rest become plain strings passed through `IEventSink`.
Nothing is lost by this: the project installs no `QTranslator` and ships no
FastECU translation catalogs, so no string here is translated at runtime today.
(The `.ts`/`.qm` files under `resources/desktop/hexedit/translations/` belong to
the vendored hex-editor widget and are untouched.)

### Move a — `read_menu_file` splits in two

`FileActions::read_menu_file` (`file_actions.cpp:796-1005`, about 210 lines)
parses `resources/shared/config/menu.cfg` with `QDomDocument` and, in the same
loop, builds `QMenu`, `QAction`, `QToolBar`, and a `QSignalMapper`. It splits
along that seam:

- **Portable model + parser:** new `src/backend/config/menu_definition.{h,cpp}`
  parses the file with pugixml into a `MenuDefinition` — nested menus, each
  item carrying id, name, checkable, shortcut, toolbar, icon, tooltip, and a
  separator flag. It lands in `src/backend/config` and mirrors
  `load_protocol_catalog` exactly, down to the signature:

  ```cpp
  fastecu::Result<MenuDefinition>
  load_menu_definition(const ConfigPaths& paths, IFileRepository& file_repository);
  ```

  `ConfigPaths::menu_file` already exists and already resolves to
  `<config>/menu.cfg`, so no new path plumbing is needed. Bytes come from
  `IFileRepository::read`, never from `QFile`; a parse failure returns
  `ErrorKind::InvalidConfig`, matching `protocol_catalog.cpp`.
- **UI builder:** new `src/ui/desktop/menu/menu_builder.{h,cpp}` walks a
  `MenuDefinition` and constructs the widget tree and `QSignalMapper`.
  `MainWindow` calls it at `mainwindow.cpp:200`, where it currently calls
  `read_menu_file(ui->menubar, ui->toolBar)`.

`FileActions::read_menu_file` is deleted. `MainWindow` is its only caller.

This parser has never had a test, because it was unreachable without a live
`QWidget`. Splitting it is what makes 210 lines testable.

### Move b — the definition-chooser workflows move to the UI

`create_new_definition_for_rom` (`file_actions.cpp:1063-1188`) and
`use_existing_definition_for_rom` (`1188-1357`) are about 294 lines of
`QFileDialog` plus a field-collecting `QDialog`. They move wholesale to a new
`src/ui/desktop/definition/definition_authoring_dialog.{h,cpp}`.

The backend half already exists: the private `submit_new_definition` and
`submit_imported_definition` take a
`fastecu::definition::DefinitionHeaderInput` and are promoted to public. The
dialog collects the header fields and calls them.

This is deliberately **not** modeled as an `IDefinitionPrompt` port. The flow
is a file chooser plus a form — presentation end to end — with exactly one
consumer. A port here would turn a wizard into an interface for a single
caller, which the step-5 umbrella's YAGNI discipline rules out.
`apply_missing_definition_defaults` stays in the backend; it contains no UI.
`MainWindow::prompt_for_missing_definition` (`mainwindow.cpp:1470-1484`) is the
call site for all three.

### Move c — 18 message boxes become `IEventSink` calls

Fifteen `QMessageBox::warning` sites in `file_actions.cpp` and three in
`file_actions_romraider.cpp` are replaced. `event_sink.h` already adjudicates
this design in its own contract comment: *"Replaces backend Qt signals and
every backend QMessageBox. Interactive confirmation is modeled where a consumer
first needs it (step 5c) as a typed request answered by the UI, not as a
blocking backend prompt."*

All eighteen are `QMessageBox::warning` and all are modal, so the mapping is
uniform and needs no per-site judgment: **every one becomes
`events_.notice(message)`**, which the UI renders as the same modal warning.
Splitting some of them onto `events_.log(LogLevel::Warning, ...)` would
silently drop a modal the user sees today, so this design does not do that.
Separately, the 38 `emit LOG_*` statements become `events_.log(level, message)`
with the level carried over unchanged.

`MainWindow` constructs a `QtEventSink`, passes it to `FileActions`, and
connects `QtEventSink::noticed` to a `QMessageBox::warning` and
`QtEventSink::logged` to `SystemLogger::log_messages`. That is the pattern
already in service at `src/platform/desktop/common/flash/flash_worker.cpp:60`.
The four `connect` calls at `mainwindow.cpp:107-110` collapse to two.

Modality is preserved: every warning the user sees today, they still see.

### Move d — `//src/algorithms/expression:qt_compat` is deleted

`FileActions::parse_stringlist_from_expression_string` and
`calculate_value_from_expression` (`file_actions.cpp:1544-1553`) are five-line
delegations onto the shim, whose only Bazel reverse dependency is
`//src/backend/definitions`. Their nine real call pairs all live in
`src/ui/desktop/menu_actions.cpp`, a file the drain has never touched.

Both methods are deleted from `FileActions`. `menu_actions.cpp` calls
`ExpressionEvaluator` from `//src/algorithms/expression` directly, converting
`QString` at the call boundary. `float_precision` stays a public member, read
by the caller. `qt_expression_evaluator.{h,cpp}`, the `qt_compat` target, and
the `test_expression_evaluator` target are deleted; the portable
`expression_evaluator_test` remains.

`//src/algorithms/menu:qt_compat` is **out of scope.** Its callers are
`MainWindow`'s own command dispatch, which is later step-6 work.

## Completion criteria

Exact and machine-checked.

- `grep -rE 'QMessageBox|QFileDialog|QDialog|QWidget|Q_OBJECT' src/backend
  --include=*.h --include=*.cpp` matches only comment lines. Enforced by a new
  `//:backend_no_widgets` `py_test`, in the shape of
  `scripts/check-serial-compat-allowlist.py` but with **no allowlist** — none
  is needed, because a full-tree scan today finds real widget code only in
  `file_actions.{h,cpp}` and `file_actions_romraider.cpp`; every other hit in
  `src/backend/**` is already a comment.
- `//src/backend/definitions:definitions` does not depend on
  `@rules_qt//:qt_widgets`, and its four `fastecu_qttest` targets no longer set
  `-DQT_WIDGETS_LIB` or `QT_QPA_PLATFORM=offscreen`.
- `//src/algorithms/expression:qt_compat` does not exist, and neither do
  `qt_expression_evaluator.h`, `qt_expression_evaluator.cpp`, or
  `expression_evaluator_qt_compat_test.cpp`.
- `class FileActions` declares no base class and no `Q_OBJECT`.
- The branch's `git diff --name-only origin/master` contains no path under
  `src/platform/desktop/common/flash/legacy/` or `src/ui/desktop/flash/`.

## Testing

- New portable `src/backend/config/menu_definition_test.cpp`
  (`fastecu_portable_gtest`): golden-parses the real
  `resources/shared/config/menu.cfg`, plus malformed XML, a missing file, an
  unknown tag, and items with each optional attribute absent. This is net-new
  coverage over 210 lines that were previously untestable.
- New `src/ui/desktop/menu/menu_builder_test.cpp` (`fastecu_qttest`,
  offscreen): asserts that building a hand-constructed `MenuDefinition`
  produces the expected `QMenuBar`/`QToolBar` tree, action object names, and
  `QSignalMapper` mappings.
- The existing `test_file_actions_parsing` (69k), `test_ecuflash_definition_parsing`,
  `test_rom_transformations`, and `test_model_validation` are the regression
  net for everything that stays. They must pass unchanged apart from dropping
  their widget deps and `offscreen` env.
- `fastecu::RecordingEventSink` (already in `src/backend/ports/testing/`)
  asserts the level and text of each notice that replaced a message box, in the
  existing parsing suites.
- `definition_authoring_dialog`'s header-field assembly is extracted as a free
  function and tested headlessly; the modal wiring itself is left untested,
  consistent with existing desktop UI practice.

## Sequencing

Five PRs, each independently mergeable and each green on `bazel test
--config=release //...` before the next starts.

1. **6a-1** — menu split: `MenuDefinition` model, pugixml parser, portable
   tests, `MenuBuilder`, builder test, `MainWindow` call swap. Deletes
   `read_menu_file`.
2. **6a-2** — `DefinitionAuthoringDialog`; `submit_new_definition` and
   `submit_imported_definition` promoted to public; `MainWindow` call swap.
   Deletes both chooser workflows from `FileActions`.
3. **6a-3** — the 18 message boxes and 38 `emit LOG_*` become `IEventSink`
   calls; `QWidget`/`Q_OBJECT`/signals dropped; remaining `tr()` removed;
   `MainWindow` wiring rewired; `definitions` drops `qt_widgets`.
4. **6a-4** — expression shim drained from `menu_actions.cpp` and deleted.
5. **6a-5** — `//:backend_no_widgets` guard; status updates to
   [the modularization plan](../../modularization-plan.md) and
   [the tech-debt roadmap](../../tech-debt.md).

**6a-3 is the only PR that races the drain**, and only inside
`mainwindow.cpp`'s constructor and signal-wiring region (lines 62-123). The
drain edits the flash-dialog include block in `mainwindow.h` and the dispatch
branches lower in `mainwindow.cpp`. The two large extractions land first
precisely so this one PR is small when it arrives. If a wave merges first, the
rebase is confined to that wiring block.

## Risks

- **`FileActions` is constructed with a `QWidget *parent` today**
  (`mainwindow.cpp:62`), which owns its lifetime. Dropping `QWidget` makes
  lifetime the caller's problem; `MainWindow` must hold it in a
  `std::unique_ptr` member. This is a one-line ownership change in the one file
  that constructs it, and is part of 6a-3.
- **6a-1 changes the XML parser** from `QDomDocument` to pugixml for
  `menu.cfg`. The golden test against the real shipped file is what makes that
  safe; any attribute-defaulting difference shows up there rather than as a
  missing menu item at runtime.
- No path in this step touches flashing or logging wire behavior, so no bench
  re-qualification is required. The
  [flash qualification matrix](../../flash-qualification-matrix.md) is
  unaffected.
